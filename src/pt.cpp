#include <locale>
#include <stdlib.h>
#include <tuple>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <string>
#include <utf8.h>
#include <date/date.h>
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"

//
// Compatibility check
//
PG_MODULE_MAGIC;


//
//
//
PG_FUNCTION_INFO_V1( ptdir );

namespace {

    //
    // POSIX already stores paths in UTF-8
    //
    template <typename CharT, size_t szChar = sizeof( CharT )>
    std::string str_2_utf8( std::basic_string<CharT>&& str ) {
        return std::move( str );
    }

    //
    // Windows uses UTF-16 for path representation
    //
    template <>
    std::string str_2_utf8<wchar_t, 2>( std::wstring&& wstr ) {
        std::string str;
        utf8::utf16to8( wstr.begin(), wstr.end(), std::back_inserter( str ) );
        return str;
    }

    //
    // Just in case
    //
    template <>
    std::string str_2_utf8<wchar_t, 4>( std::wstring&& wstr ) {
        std::string str;
        utf8::utf32to8( wstr.begin(), wstr.end(), std::back_inserter( str ) );
        return str;
    }

    //
    // Safe wrapper for memory block allocated by palloc()
    //
    struct pmem {
        pmem() noexcept = default;
        explicit pmem( void* ptr ) noexcept : _ptr( ptr ) {}
        pmem( const pmem& ) = delete;
        pmem( pmem&& other ) noexcept { other.swap( *this ); }
        ~pmem() { if ( _ptr ) pfree( _ptr ); }
        pmem& operator=( const pmem& ) = delete;
        pmem& operator=( pmem&& rhs ) noexcept { pmem{}.swap( *this ); rhs.swap( *this ); return *this; }
        operator bool() const noexcept { return _ptr; }
        template < typename T > explicit operator T* ( ) const noexcept { return static_cast< T* >( _ptr ); }
        void swap( pmem& other ) noexcept { std::swap( _ptr, other._ptr ); }
        friend void swap( pmem& lhs, pmem& rhs ) noexcept { lhs.swap( rhs ); }
    private:
        void* _ptr = nullptr;
    };

    using file_info_type = std::tuple<
        std::string,  // file attributes
        int64_t,      // file size
        std::string,  // file time
        std::string,  // file type
        std::string   // file name
    >;

    file_info_type get_file_info( std::filesystem::directory_iterator& dit ) {

        using std::filesystem::perms;
        auto status = dit->is_symlink() ? dit->symlink_status() : dit->status();
        auto permissions = status.permissions();
        std::stringstream fattr;
        fattr << ( dit->is_directory() ? 'd' : '-' )
            << ( perms::none == ( permissions & perms::owner_read   ) ? '-' : 'r' )
            << ( perms::none == ( permissions & perms::owner_write  ) ? '-' : 'w' )
            << ( perms::none == ( permissions & perms::owner_exec   ) ? '-' : 'x' )
            << ( perms::none == ( permissions & perms::group_read   ) ? '-' : 'r' )
            << ( perms::none == ( permissions & perms::group_write  ) ? '-' : 'w' )
            << ( perms::none == ( permissions & perms::group_exec   ) ? '-' : 'x' )
            << ( perms::none == ( permissions & perms::others_read  ) ? '-' : 'r' )
            << ( perms::none == ( permissions & perms::others_write ) ? '-' : 'w' )
            << ( perms::none == ( permissions & perms::others_exec  ) ? '-' : 'x' );

        using namespace std::filesystem;
        using namespace std::chrono;
        auto tp = time_point_cast< system_clock::duration >( dit->last_write_time() - file_time_type::clock::now() + system_clock::now() );

        using namespace std::string_literals;
        using std::filesystem::file_type;
        std::string ftype;
        switch ( status.type() ) {
            case file_type::block: {
                static const auto type = "S_ISBLK"s;
                ftype = type;
            } break;
            case file_type::character: {
                static const auto type = "S_ISCHR"s;
                ftype = type;
            } break;
            case file_type::fifo: {
                static const auto type = "S_ISFIFO"s;
                ftype = type;
            } break;
            case file_type::socket: {
                static const auto type = "S_IFSOCK"s;
                ftype = type;
            } break;
            default: break;
        }

        std::stringstream fname;
        fname << str_2_utf8( dit->path().filename().string() );
        if ( dit->is_symlink() ) {
            static const auto pointer = " --> "s;
            fname << pointer << str_2_utf8( std::filesystem::read_symlink( dit->path() ).string() );
        }

        return { fattr.str(), dit->file_size(), date::format( "%D %T %Z", floor<milliseconds>( tp ) ), std::move( ftype ), fname.str() };
    }
}


PGDLLEXPORT
Datum ptdir( PG_FUNCTION_ARGS ) {
    FuncCallContext* funcctx = nullptr;
    try {

        static std::mutex guard;
        static std::unordered_map< decltype( funcctx ), std::filesystem::directory_iterator > sessions;

        //
        // INIT: stuff done only on the first call of the function
        //
        if ( SRF_IS_FIRSTCALL() ) {
            // create a function context for cross-call persistence
            funcctx = SRF_FIRSTCALL_INIT();

            // switch to memory context appropriate for multiple function calls
            auto  oldcontext = MemoryContextSwitchTo( funcctx->multi_call_memory_ctx );

            // total number of tuples to be returned
            funcctx->max_calls = PG_GETARG_INT32( 0 );

            // Build a tuple descriptor for our result type
            TupleDesc tupdesc;
            if ( get_call_result_type( fcinfo, NULL, &tupdesc ) != TYPEFUNC_COMPOSITE ) {
                ereport( ERROR, (
                    errcode( ERRCODE_FEATURE_NOT_SUPPORTED ),
                    errmsg( "function returning record called in context " "that cannot accept type record" )
                ) );
            }

            //
            // generate attribute metadata needed later to produce tuples from raw
            // C strings
            //
            funcctx->attinmeta = TupleDescGetAttInMetadata( tupdesc );

            // restore memory context
            MemoryContextSwitchTo( oldcontext );

            // create directory iterator and associate with call context
            std::unique_lock lock( guard );
            if ( !sessions.emplace( funcctx, std::filesystem::directory_entry{ PG_GETARG_CSTRING( 1 ) } ).second ) {
                throw std::runtime_error{ "duplicated session UID" };
            }
        }

        //
        // RUN: stuff done on every call of the function */
        //
        funcctx = SRF_PERCALL_SETUP();

        // get directory iterator by the call context
        decltype( sessions )::iterator session;
        {
            std::unique_lock lock( guard );
            session = sessions.find( funcctx );
            if ( session == sessions.end() ) {
                throw std::runtime_error{ "unknown session UID" };
            }
        }

        // while we have something to do
        if ( auto& dit = session->second; dit != std::filesystem::end( dit ) && funcctx->call_cntr < funcctx->max_calls ) {

            auto [fattr, fsize, ftime, ftype, fname ] = get_file_info( dit );
            ++dit;

            enum field_ids {
                field_id_fattr = 0,
                field_id_fsize,
                field_id_ftime,
                field_id_ftype,
                field_id_fname,
                field_id_max
            };

            // allocate initialize values
            pmem mem_values{ palloc( funcctx->attinmeta->tupdesc->natts * sizeof( Datum ) ) };
            pmem mem_nulls{ palloc( funcctx->attinmeta->tupdesc->natts * sizeof( bool ) ) };
            if (!mem_values || !mem_nulls ) {
                throw std::runtime_error( "Unable to allocate memory" );
            }
            auto values = (Datum*)mem_values;
            auto nulls = (bool*)mem_nulls;
            std::fill_n( nulls, funcctx->attinmeta->tupdesc->natts, false );

            // fill values or mark nulls
            values[ field_id_fattr ] = CStringGetDatum( fattr.c_str() );
            values[ field_id_fsize ] = Int64GetDatum( fsize );
            values[ field_id_ftime ] = CStringGetDatum( ftime.c_str() );
            values[ field_id_fname ] = CStringGetDatum( fname.c_str() );
            if (ftype.size()) {
                values[ field_id_ftype ] = CStringGetDatum( ftype.c_str() );
            }
            else {
                nulls[ field_id_ftype ] = true;
            }

            // make tuple from filles datums and nulls
            auto tuple = heap_form_tuple( funcctx->attinmeta->tupdesc, (Datum*)values, (bool*)nulls );

            // make resulting datum from the tuple
            auto result = HeapTupleGetDatum( tuple );

            //
            // ROW is ready: return the datum as next row in the set
            //
            SRF_RETURN_NEXT( funcctx, result );
        }
        else {
            //
            // DONE: eliminate the session and notify the caller
            //
            std::unique_lock lock( guard );
            sessions.erase( session );
            SRF_RETURN_DONE( funcctx );
        }
    }
    catch ( const std::exception& e ) {
        ereport( ERROR, (
            errcode( ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION ),
            errmsg( "dir() thrown exception: %s", e.what() )
        ) );
    }
    catch ( ... ) {
        ereport( ERROR, (
            errcode( ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION ),
            errmsg( "dir() thrown unknown exception" )
        ) );
    }

    SRF_RETURN_DONE( funcctx );
}
