#include "ten_load.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>

typedef struct StackN   StackN;
typedef struct SetN     SetN;
typedef struct Loader   Loader;
typedef struct Version  Version;
typedef struct Slice    Slice;


struct Slice {
    char*  str;
    size_t len;
};

struct StackN {
    StackN* prev;
    size_t  plen;
    char    pstr[];
};

struct SetN {
    SetN*  next;
    SetN** link;
    unsigned hash;
    size_t   plen;
    char     pstr[];
};

struct Loader {
    unsigned nlib;
    Slice*   plib;
    char*    lang;
    
    StackN* stack;
    
    #define SET_CAP (64)
    SetN* set[SET_CAP];
};

struct Version {
    long major;
    long minor;
    long patch;
};

static StackN*
pswap( Loader* ld, StackN* stack ) {
    StackN* oStack = ld->stack;
    ld->stack = stack;
    return oStack;
}

static void
pclear( Loader* ld, StackN* top ) {
    StackN* it = ld->stack;
    while( it && it != top ) {
        StackN* n = it;
        it = it->prev;
        
        free( n );
    }
    
    ld->stack = top;
}

static StackN*
ppush( Loader* ld, char const* path, size_t len ) {
    StackN* node = malloc( sizeof(StackN) + len + 1 );
    node->plen = len;
    memcpy( node->pstr, path, len );
    node->pstr[len] = '\0';
    
    node->prev = ld->stack;
    ld->stack = node;
    
    return node->prev;
}

static void
ppop( Loader* ld ) {
    assert( ld->stack );
    
    StackN* oStack = ld->stack;
    ld->stack = oStack->prev;
    free( oStack );
}

static unsigned
phash( char const* str, size_t len ) {
    unsigned h = 0;
    for( size_t i = 0 ; i < len ; i++ )
        h = h*37 + str[i];
    
    return h;
}

static bool
padd( Loader* ld, char const* path, size_t len ) {
    unsigned h = phash( path, len );
    unsigned i = h % SET_CAP;
    
    SetN* it = ld->set[i];
    while( it ) {
        if( it->plen == len && it->hash == h && !memcmp( it->pstr, path, len ) )
            return true;
        it = it->next;
    }
    
    SetN* node = malloc( sizeof(SetN) + len + 1 );
    node->next = ld->set[i];
    node->link = &ld->set[i];
    *node->link = node;
    if( node->next )
        node->next->link = &node->next;
    
    node->hash = h;
    node->plen = len;
    memcpy( node->pstr, path, len );
    node->pstr[len] = '\0';
    
    return false;
}

static void
prem( Loader* ld, char const* path, size_t len ) {
    unsigned h = phash( path, len );
    unsigned i = h % SET_CAP;
    
    SetN* it = ld->set[i];
    while( it ) {
        if( it->plen == len && it->hash == h && !memcmp( it->pstr, path, len ) )
            break;
        it = it->next;
    }
    assert( it );
    
    if( it->next )
        it->next->link = it->link;
    *it->link = it->next;
    
    free( it );
}

#define MAX_VER_LEN (64)

static bool
fillVer( ten_State* ten, Loader* ld, char const* dir, Version* ver ) {
    DIR* d = opendir( dir );
    if( d == NULL )
        return false;
    
    struct dirent* e = readdir( d );
    while( e ) {
        char* next = e->d_name;
        char* end  = NULL;
        
        long major = strtol( next, &end, 10 );
        if( end == next || *end != '-' )
            continue;
        
        if( major >= ver->major ) {
            next = end + 1;
            long minor = strtol( next, &end, 10 );
            if( end == next || *end != '-' )
                continue;
            
            if( minor >= ver->minor ) {
                next = end + 1;
                long patch = strtol( next, &end, 10 );
                if( end == next || *end != '\0' )
                    continue;
                
                if( patch > ver->patch ) {
                    ver->major = major;
                    ver->minor = minor;
                    ver->patch = patch;
                }
            }
        }
        e = readdir( d );
    }
    
    closedir( d );
    
    if( ver->minor < 0 || ver->major < 0 || ver->patch < 0 )
        return false;
    else
        return true;
}

static bool
fillExt( ten_State* ten, Loader* ld, char const* file, char const** ext ) {
    size_t flen = strlen( file );
    size_t plen = flen + 9;
    char   pstr[plen];
    char*  pext = &pstr[flen];
    memcpy( pstr, file, flen );
    
    strcpy( pext, ".ten" );
    if( access( pstr, F_OK ) != -1  ) {
        *ext = ".ten";
        return true;
    }
    
    strcpy( pext, ".so" );
    if( access( pstr, F_OK ) != -1 ) {
        *ext = ".so";
        return true;
    }
    
    strcpy( pext, ".txt" );
    if( access( pstr, F_OK ) != -1 ) {
        *ext = ".txt";
        return true;
    }
    
    strcpy( pext, ".dat" );
    if( access( pstr, F_OK ) ) {
        *ext = ".dat";
        return true;
    }
    
    static char lext[] = ".xxx.str";
    memcpy( lext + 1, ld->lang, 3 );
    
    strcpy( pext, lext );
    if( access( pstr, F_OK ) != -1 ) {
        *ext = lext;
        return true;
    }
    return false;
}

static Slice*
libFind( ten_State* ten, Loader* ld, Slice* dir, Slice* lib, Version* ver, Slice* path ) {
    size_t plen = dir->len + lib->len + MAX_VER_LEN + path->len + 16;
    char   pstr[plen];
    
    size_t loc = 0;
    memcpy( &pstr[loc], dir->str, dir->len );
    loc += dir->len;
    
    pstr[loc] = '/';
    loc += 1;
    
    memcpy( &pstr[loc], lib->str, lib->len );
    loc += lib->len;
    
    pstr[loc] = '\0';
    
    if( !fillVer( ten, ld, pstr, ver ) )
        return NULL;
    
    pstr[loc] = '/';
    loc += 1;
    
    loc +=
        sprintf(
            &pstr[loc],
            "%li-%li-%li",
            ver->major,
            ver->minor,
            ver->patch
        );
    
    pstr[loc] = '/';
    loc += 1;
    
    memcpy( &pstr[loc], path->str, path->len );
    loc += path->len;
    
    pstr[loc] = '\0';
    char const* ext = NULL;
    if( !fillExt( ten, ld, pstr, &ext ) )
        return NULL;
    
    loc += sprintf( &pstr[loc], "%s", ext );
    
    
    static Slice spath = { .str = NULL, .len = 0 };
    spath.len = loc;
    spath.str = realloc( spath.str, spath.len );
    memcpy( spath.str, pstr, loc );
    
    return &spath;
}


static Slice*
proFind( ten_State* ten, Loader* ld, Slice* dir, Slice* path ) {
    size_t plen = dir->len + path->len + 10;
    char   pstr[plen];
    
    size_t loc = 0;
    memcpy( &pstr[loc], dir->str, dir->len );
    loc += dir->len;
    
    pstr[loc] = '/';
    loc += 1;
    
    memcpy( &pstr[loc], path->str, path->len );
    loc += path->len;
    
    pstr[loc] = '\0';
    char const* ext = NULL;
    if( !fillExt( ten, ld, pstr, &ext ) )
        return NULL;
    
    loc += sprintf( &pstr[loc], "%s", ext );
    
    
    static Slice spath = { .str = NULL, .len = 0 };
    spath.len = loc;
    spath.str = realloc( spath.str, spath.len );
    memcpy( spath.str, pstr, loc );
    
    return &spath;
}

typedef void (*ExportFun)( ten_State* ten, ten_Var* export );

static void
soLoad( ten_State* ten, Loader* ld, char const* path, ten_Var* dst ) {
    void* dll = dlopen( path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE );
    if( !dll )
        ten_panic( ten, ten_str( ten, dlerror() ) );
    
    ExportFun ten_export = dlsym( dll, "ten_export" );
    if( !ten_export )
        ten_panic( ten, ten_str( ten, "No 'ten_export' function in DLL" ) );
    
    
    ten_Tup varTup = ten_pushA( ten, "U" );
    ten_Var idxVar = { .tup = &varTup, .loc = 0 };
    
    ten_newIdx( ten, &idxVar );
    ten_newRec( ten, &idxVar, dst );
    
    ten_export( ten, dst );
    
    ten_pop( ten );
}

static void
tenLoad( ten_State* ten, Loader* ld, char const* path, ten_Var* dst ) {
    ten_Tup argTup = ten_pushA( ten, "" );
    
    ten_Tup varTup = ten_pushA( ten, "UU" );
    ten_Var idxVar = { .tup = &varTup, .loc = 0 };
    ten_Var clsVar = { .tup = &varTup, .loc = 1 };
    
    ten_newIdx( ten, &idxVar );
    ten_newRec( ten, &idxVar, dst );
    
    char const* upvals[] = { "export", NULL };
    ten_Source* src = ten_pathSource( ten, path );
    ten_compileExpr( ten, upvals, src, ten_SCOPE_LOCAL, ten_COM_CLS, &clsVar );
    
    
    ten_setUpvalue( ten, &clsVar, 0, dst );
    ten_call( ten, &clsVar, &argTup );
    ten_pop( ten );
}

static void
rawLoad( ten_State* ten, Loader* ld, char const* path, ten_Var* dst ) {
    struct stat buf;
    if( stat( path, &buf ) < 0 )
        ten_panic( ten, ten_str( ten, strerror( errno ) ) );
    
    FILE* file = fopen( path, "r" );
    if( !file )
        ten_panic( ten, ten_str( ten, strerror( errno ) ) );
    
    
    char*  raw = malloc( buf.st_size );
    size_t len = fread( raw, 0, buf.st_size, file );
    fclose( file );
    
    ten_newStr( ten, raw, len, dst );
}

static bool
checkPath( char const* path, size_t len ) {
    if( !strncmp( path, "/", 1 ) )
        return false;
    if( !strncmp( path, "./", 2 ) )
        return false;
    if( !strncmp( path, "../", 3 ) )
        return false;
    if( !strncmp( path, "~/", 2 ) )
        return false;
    
    for( size_t i = 0 ; i < len ; i++ ) {
        if( path[i] != '/' )
            continue;
        
        if( !strncmp( &path[i], "//", 2 ) )
            return false;
        if( !strncmp( &path[i], "/./", 3 ) )
            return false;
        if( !strncmp( &path[i], "/../", 4 ) )
            return false;
        if( !strncmp( &path[i], "/~/", 3 ) )
            return false;
    }
    return true;
}

static ten_Tup
libTrans( ten_PARAMS ) {
    Loader* ld = (Loader*)dat;
    
    ten_Var modArg = { .tup = args, .loc = 0 };
    ten_expect( ten, "mod", ten_sym( ten, "Str" ), &modArg );
    
    size_t      len      = ten_getStrLen( ten, &modArg );
    char const* modStart = ten_getStrBuf( ten, &modArg );
    char const* modEnd   = modStart + len;
    
    // Parse the package/library name, this is everything
    // from the start of the module id to either a '#'
    // character which marks the version or a '/' which
    // marks the first next path element.
    char const* libStart = modStart;
    char const* libEnd   = modStart;
    while( libEnd != modEnd && *libEnd != '#' && *libEnd != '/' )
        libEnd++;
    
    // Parse the version, it's implicitly set to whatever is
    // the latest version availabile { -1, -1, -1 } unless
    // one or more elements of the semantic version are
    // specified after the '#'.
    Version ver = { -1, -1, -1 };
    char* verEnd = (char*)libEnd;
    if( *libEnd == '#' ) {
        char* verNext  = (char*)libEnd + 1;
        
        ver.major = strtol( verNext, &verEnd, 10 );
        if( *verEnd == '.' ) {
            verNext = verEnd + 1;
            ver.minor = strtol( verNext, &verEnd, 10 );
            if( *verEnd == '.' ) {
                verNext = verEnd + 1;
                ver.patch = strtol( verNext, &verEnd, 10 );
                if( *verEnd == '.' )
                    ten_panic( ten, ten_str( ten, "Extra version component" ) );
            }
        }
        if( verEnd == verNext )
            ten_panic( ten, ten_str( ten, "Invalid version component" ) );
        if( verEnd != modEnd && *verEnd != '/' )
            ten_panic(
                ten,
                ten_str(
                    ten,
                    "Version followed by something other "
                    "than path component"
                )
            );
    }
    else
    if( verEnd != modEnd && *verEnd != '/' )
        ten_panic(
            ten,
            ten_str(
                ten,
                "Library name followed by something other "
                "than version or path component"
            )
        );
    
    
    char const* pathStart = verEnd + 1;
    char const* pathEnd   = modEnd;
    
    if( !checkPath( pathStart, pathEnd - pathStart ) )
        ten_panic( ten, ten_str( ten, "Invalid module path" ) );
    
    Slice lib = {
        .str = (char*)libStart,
        .len = libEnd - libStart
    };
    Slice path = {
        .str = (char*)pathStart,
        .len = pathEnd - pathStart
    };
    
    Slice* fpath = NULL;
    for( unsigned i = 0 ; i < ld->nlib ; i++ ) {
        fpath = libFind( ten, ld, &ld->plib[i], &lib, &ver, &path );
        if( fpath )
            break;
    }
    
    ten_Tup retTup = ten_pushA( ten, "U" );
    ten_Var retVar = { .tup = &retTup, .loc = 0 };
    if( fpath )
        ten_newStr( ten, fpath->str, fpath->len, &retVar );
    
    return retTup;
}

static ten_Tup
proTrans( ten_PARAMS ) {
    Loader* ld = (Loader*)dat;
    
    ten_Var modArg = { .tup = args, .loc = 0 };
    ten_expect( ten, "mod", ten_sym( ten, "Str" ), &modArg );
    
    size_t      len      = ten_getStrLen( ten, &modArg );
    char const* modStart = ten_getStrBuf( ten, &modArg );
    char const* modEnd   = modStart + len;
    
    if( !checkPath( modStart, len ) )
        ten_panic( ten, ten_str( ten, "Invalid module path" ) );
    
    assert( ld->stack );
    Slice  spath = { .str = (char*)modStart, .len = len };
    Slice  sdir  = { .str = ld->stack->pstr, .len = ld->stack->plen };
    Slice* fpath = proFind( ten, ld, &sdir, &spath );
    
    ten_Tup retTup = ten_pushA( ten, "U" );
    ten_Var retVar = { .tup = &retTup, .loc = 0 };
    if( fpath )
        ten_newStr( ten, fpath->str, fpath->len, &retVar );
    
    return retTup;
}

static ten_Tup
load( ten_PARAMS ) {
    Loader* ld = (Loader*)dat;
    
    ten_Var pathArg = { .tup = args, .loc = 0 };
    ten_expect( ten, "path", ten_sym( ten, "Str" ), &pathArg );
    
    size_t      plen = ten_getStrLen( ten, &pathArg );
    char const* pstr = ten_getStrBuf( ten, &pathArg );
    char const* pend = pstr + plen;
    
    // Add the library module to the set of modules
    // being imported, this is used for cycle detection,
    // if the module is already in the set then there's
    // a dependency cycle somewhere, so throw an error.
    if( padd( ld, pstr, plen ) )
        ten_panic( ten, ten_str( ten, "Dependency cycle detected" ) );
    
    // Figure out where the parent directory path ends.
    long long dlen = plen;
    while( dlen >= 0 && pstr[dlen] != '/' )
        dlen--;
    assert( dlen > 0 );
    
    // Push the module directory to the stack as the new
    // module root, saving the old stack top to be restored
    // later.
    StackN* oTop = ppush( ld, pstr, dlen );
    
    jmp_buf  jmp;
    jmp_buf* oJmp = ten_swapErrJmp( ten, &jmp );
    int sig = setjmp( jmp );
    if( sig ) {
        pclear( ld, oTop );
        prem( ld, pstr, plen );
        ten_swapErrJmp( ten, oJmp );
        ten_propError( ten, NULL );
        assert( false );
    }
    
    ten_Tup retTup = ten_pushA( ten, "U" );
    ten_Var retVar = { .tup = &retTup, .loc = 0 };
    
    if( plen > 3 && !strcmp( pend - 3, ".so" ) )
        soLoad( ten, ld, pstr, &retVar );
    else
    if( plen > 4 && !strcmp( pend - 4, ".ten" ) )
        tenLoad( ten, ld, pstr, &retVar );
    else
    if( plen > 4 && !strcmp( pend - 4, ".dat" ) )
        rawLoad( ten, ld, pstr, &retVar );
    else
    if( plen > 4 && !strcmp( pend - 4, ".txt" ) )
        rawLoad( ten, ld, pstr, &retVar );
    else
    if( plen > 4 && !strcmp( pend - 4, ".str" ) )
        rawLoad( ten, ld, pstr, &retVar );
    else
        ten_panic( ten, ten_str( ten, "Bad tranlsation" ) );
    
    prem( ld, pstr, plen );
    pclear( ld, oTop );
    ten_swapErrJmp( ten, oJmp );
    
    return retTup;
}

void
finl( ten_State* ten, void* dat ) {
    Loader* ld = dat;
    pclear( ld, NULL );
    for( unsigned i = 0 ; i < SET_CAP ; i++ ) {
        SetN* nIt = ld->set[i];
        while( nIt ) {
            SetN* n = nIt;
            nIt = nIt->next;
            
            free( n );
        }
    }
    free( ld->plib );
    free( ld->lang );
}

void
ten_load( ten_State* ten, char const* ppro, char const** plib, char const* lang ) {
    if( strlen( lang ) > 3 ) {
        fprintf( stderr, "ten_load: Language tag 'lang' is too long, must be 3 chars" );
        exit( 1 );  
    }
    if( ppro[0] != '/' ) {
        fprintf( stderr, "ten_load: Project path 'ppro' must be absolute path" );
        exit( 1 );  
    }
    
    unsigned nlib = 0;
    for( unsigned i = 0 ; plib[i] != NULL ; i++ ) {
        nlib++;
        if( plib[i][0] != '/' ) {
            fprintf( stderr, "ten_load: Library path 'plib[%u]' must be absolute path", i );
            exit( 1 );  
        }
    }
    
    
    ten_Tup varTup    = ten_pushA( ten, "UUUUUSS", "pro", "lib" );
    ten_Var datVar    = { .tup = &varTup, .loc = 0 };
    ten_Var funVar    = { .tup = &varTup, .loc = 1 };
    ten_Var loadVar   = { .tup = &varTup, .loc = 2 };
    ten_Var ptransVar = { .tup = &varTup, .loc = 3 };
    ten_Var ltransVar = { .tup = &varTup, .loc = 4 };
    ten_Var ptypeVar  = { .tup = &varTup, .loc = 5 };
    ten_Var ltypeVar  = { .tup = &varTup, .loc = 6 };
    
    ten_DatInfo* ldInfo =
        ten_addDatInfo(
            ten,
            &(ten_DatConfig){
                .tag   = "Loader",
                .size  = sizeof(Loader),
                .mems  = 0,
                .destr = finl
            }
        );
    Loader* ld = ten_newDat( ten, ldInfo, &datVar );
    
    ld->lang = malloc( 4 );
    strcpy( ld->lang, lang );
    
    ld->nlib = nlib;
    ld->plib = malloc( sizeof(Slice)*nlib );
    for( unsigned i = 0 ; plib[i] != NULL ; i++ ) {
        size_t len = strlen( plib[i] );
        char*  str = malloc( len + 1 );
        strcpy( str, plib[i] );
        
        ld->plib[i].len = len;
        ld->plib[i].str = str;
    }
    
    ld->stack = NULL;
    
    for( unsigned i = 0 ; i < SET_CAP ; i++ )
        ld->set[i] = NULL;
    
    ten_FunParams pTransParams = {
        .name   = "proTrans",
        .params = (char const*[]){ "mod", NULL },
        .cb     = proTrans
    };
    ten_newFun( ten, &pTransParams, &funVar );
    ten_newCls( ten, &funVar, &datVar, &ptransVar );
    
    ten_FunParams lTransParams = {
        .name   = "libTrans",
        .params = (char const*[]){ "mod", NULL },
        .cb     = libTrans
    };
    ten_newFun( ten, &lTransParams, &funVar );
    ten_newCls( ten, &funVar, &datVar, &ltransVar );
    
    ten_FunParams loadParams = {
        .name   = "load",
        .params = (char const*[]){ "path", NULL },
        .cb     = load
    };
    ten_newFun( ten, &loadParams, &funVar );
    ten_newCls( ten, &funVar, &datVar, &loadVar );
    
    ten_loader( ten, &ptypeVar, &loadVar, &ptransVar );
    ten_loader( ten, &ltypeVar, &loadVar, &ltransVar );
    
    ten_pop( ten );
}
