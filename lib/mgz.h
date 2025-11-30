#pragma once

/*
TODO
    add a mgz config data structure for setting up custom malloc,
    custom file reader, etc.
    but it needs to be thread safe in some way and i don't won't to use locks over it
*/

#define FCT_NOTE(n) [[clang::annotate(n)]]
#define MGZ FCT_NOTE("mgz")
