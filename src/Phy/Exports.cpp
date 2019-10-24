// BSD 3-Clause License

// Copyright (c) 2019, SCALE Lab, Brown University
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.

// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.

// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "Exports.hpp"
#include <PhyKnight/Phy/Phy.hpp>
#include "PhyLogger/PhyLogger.hpp"

namespace phy
{

int
read_def(const char* def_path)
{
    return Phy::instance().readDef(def_path);
}
int
read_lef(const char* lef_path)
{
    return Phy::instance().readLef(lef_path);
}
int
read_lib(const char* lib_path)
{
    return read_liberty(lib_path);
}
int
read_liberty(const char* lib_path)
{
    return Phy::instance().readLib(lib_path);
}
int
write_def(const char* lib_path)
{
    return Phy::instance().writeDef(lib_path);
}

void
version()
{
    print_version();
}

int
transform_internal(std::string transform_name, std::vector<std::string> args)
{
    return Phy::instance().runTransform(transform_name, args);
}
void
help()
{
    print_usage();
}

void
print_usage()
{
    Phy::instance().printUsage();
}

void
print_version()
{
    Phy::instance().printVersion();
}

Database&
get_database()
{
    return *(Phy::instance().database());
}
Liberty&
get_liberty()
{
    return *(Phy::instance().liberty());
}

int
set_log(const char* level)
{
    return set_log_level(level);
}

int
set_log_level(const char* level)
{
    return Phy::instance().setLogLevel(level);
}

int
set_log_pattern(const char* pattern)
{
    return Phy::instance().setLogPattern(pattern);
}

SteinerTree*
create_steiner_tree(const char* pin_name)
{
    return nullptr;
}

int
print_liberty_cells()
{
    Liberty* liberty = Phy::instance().liberty();
    if (!liberty)
    {
        PhyLogger::instance().error("Did not find any liberty files, use "
                                    "read_liberty <file name> first.");
        return -1;
    }
    sta::LibertyCellIterator cell_iter(liberty);
    while (cell_iter.hasNext())
    {
        sta::LibertyCell* cell = cell_iter.next();
        PhyLogger::instance().info("Cell: {}", cell->name());
    }
    return 1;
}

} // namespace phy
