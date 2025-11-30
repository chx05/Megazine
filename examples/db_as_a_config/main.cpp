#include "../../lib/mgz.h"

#include <stdint.h>
#include <iostream>

#define DUMP(expr) std::cout << #expr << "\n↳ " << (expr) << "\n\n"

#define REPR FCT_NOTE("struct_repr_inline")

struct MGZ REPR Config
{
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t w;
};

#include "g/mgz.g.h"
#include "g/repr.g.h"

int main()
{
    auto cfg = Config { .x=0, .y=1, .z=2, .w=3 };
    g::mgz::store<Config>("config.mgz", cfg);

    auto cfg_again = g::mgz::load<Config>("config.mgz");

    DUMP(g::repr(cfg));
    DUMP(g::repr(cfg_again));

    return 0;
}
