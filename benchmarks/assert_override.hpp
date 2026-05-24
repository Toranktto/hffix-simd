#pragma once

#include <cstdlib>

#ifdef HFFIX_ASSERT
#error "Include assert_override.hpp before <hffix.hpp>"
#endif

#define HFFIX_ASSERT(cond, msg) \
    do {                        \
        if (!(cond))            \
            std::abort();       \
    } while (0)
