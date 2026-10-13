// ps5-spotify - Probe: do C++ exceptions work in this native-app runtime?
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built with -fexceptions -frtti (tools/build.sh, src/exceptions/), because
// cspot throws. Checks what cspot relies on: a throw crossing a function
// boundary, catch by base class (needs RTTI), destructors run while unwinding,
// and a rethrow from catch(...).

#include <stdexcept>
#include <string>

namespace
{

int destroyed = 0;

struct Guard
{
    ~Guard() { destroyed++; }
};

[[gnu::noinline]] void thrower(int depth)
{
    Guard guard;
    if (depth == 0)
        throw std::runtime_error("probe");
    thrower(depth - 1);
}

} // namespace

// Returns 0 when everything behaved, else the number of the first failed check.
extern "C" int spt_exception_probe()
{
    try
    {
        thrower(3);
        return 1; // no throw
    }
    catch (const std::exception &error)
    {
        if (std::string(error.what()) != "probe")
            return 2;
    }
    if (destroyed != 4)
        return 3; // unwinding skipped destructors

    try
    {
        try
        {
            throw 42;
        }
        catch (...)
        {
            throw;
        }
    }
    catch (int value)
    {
        return value == 42 ? 0 : 4;
    }
    return 5;
}
