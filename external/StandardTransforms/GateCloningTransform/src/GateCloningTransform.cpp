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

#include "GateCloningTransform.hpp"
#include <OpenPhySyn/PsnLogger/PsnLogger.hpp>
#include <OpenPhySyn/Utils/PsnGlobal.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

using namespace psn;

GateCloningTransform::GateCloningTransform() : net_index_(0), clone_index_(0)
{
}
int
GateCloningTransform::gateClone(Psn* psn_inst, float cap_factor,
                                bool clone_largest_only)
{
    clone_count_             = 0;
    PsnLogger&       logger  = PsnLogger::instance();
    DatabaseHandler& handler = *(psn_inst->handler());
    logger.debug("Clone {} {}", cap_factor, clone_largest_only);
    std::vector<InstanceTerm*> level_drvrs = handler.levelDriverPins();
    for (auto& pin : level_drvrs)
    {
        Instance* inst = handler.instance(pin);
        cloneTree(psn_inst, inst, cap_factor, clone_largest_only);
        // if (handler.area() > psn_inst->settings()->maxArea())
        // {
        //     logger.warn("Max utilization reached!");
        //     break;
        // }
    }
    return clone_count_;
}
void
GateCloningTransform::cloneTree(Psn* psn_inst, Instance* inst, float cap_factor,
                                bool clone_largest_only)
{
    PsnLogger&       logger  = PsnLogger::instance();
    DatabaseHandler& handler = *(psn_inst->handler());
    float cap_per_micron     = psn_inst->settings()->capacitancePerMicron();

    auto output_pins = handler.outputPins(inst);
    if (!output_pins.size())
    {
        return;
    }
    InstanceTerm* output_pin = *(output_pins.begin());
    Net*          net        = handler.net(output_pin);
    if (!net)
    {
        return;
    }
    std::unique_ptr<SteinerTree> tree = SteinerTree::create(net, psn_inst);
    if (tree == nullptr)
    {
        return;
    }

    float        total_net_load = tree->totalLoad(cap_per_micron);
    LibraryCell* cell           = handler.libraryCell(inst);

    float output_target_load = handler.targetLoad(cell);

    float c_limit = cap_factor * output_target_load;
    logger.trace("{} {} output_target_load: {}", handler.name(inst),
                 handler.name(cell), output_target_load);
    logger.trace("{} {} cap_per_micron: {}", handler.name(inst),
                 handler.name(cell), cap_per_micron);
    logger.trace("{} {} c_limit: {}", handler.name(inst), handler.name(cell),
                 c_limit);
    logger.trace("{} {} total_net_load: {}", handler.name(inst),
                 handler.name(cell), total_net_load);
    if ((c_limit - total_net_load) > std::numeric_limits<float>::epsilon())
    {
        logger.trace("{} {} load is fine", handler.name(inst),
                     handler.name(cell));
        return;
    }
    if (clone_largest_only && cell != handler.largestLibraryCell(cell))
    {
        logger.trace("{} {} is not the largest cell", handler.name(inst),
                     handler.name(cell));
        return;
    }

    int fanout_count = handler.fanoutPins(net).size();

    if (fanout_count <= 1)
    {
        return;
    }
    logger.debug("Cloning: {}", handler.name(inst), handler.name(cell));

    topDownClone(psn_inst, tree, tree->driverPoint(), c_limit);
}
void
GateCloningTransform::topDownClone(psn::Psn*                          psn_inst,
                                   std::unique_ptr<psn::SteinerTree>& tree,
                                   psn::SteinerPoint k, float c_limit)
{
    DatabaseHandler& handler = *(psn_inst->handler());
    float cap_per_micron     = psn_inst->settings()->capacitancePerMicron();

    SteinerPoint drvr = tree->driverPoint();

    float src_wire_len = handler.dbuToMeters(tree->distance(drvr, k));
    float src_wire_cap = src_wire_len * cap_per_micron;
    if (src_wire_cap > c_limit)
    {
        return;
    }

    SteinerPoint left  = tree->left(k);
    SteinerPoint right = tree->right(k);
    if (left != SteinerNull)
    {
        float cap_left = tree->subtreeLoad(cap_per_micron, left) + src_wire_cap;
        bool  is_leaf =
            tree->left(left) == SteinerNull && tree->right(left) == SteinerNull;
        if (cap_left < c_limit || is_leaf)
        {
            cloneInstance(psn_inst, tree, left);
        }
        else
        {
            topDownClone(psn_inst, tree, left, c_limit);
        }
    }

    if (right != SteinerNull)
    {
        float cap_right =
            tree->subtreeLoad(cap_per_micron, right) + src_wire_cap;
        bool is_leaf = tree->left(right) == SteinerNull &&
                       tree->right(right) == SteinerNull;
        if (cap_right < c_limit || is_leaf)
        {
            cloneInstance(psn_inst, tree, right);
        }
        else
        {
            topDownClone(psn_inst, tree, right, c_limit);
        }
    }
}
void
GateCloningTransform::topDownConnect(psn::Psn* psn_inst,
                                     std::unique_ptr<psn::SteinerTree>& tree,
                                     psn::SteinerPoint k, psn::Net* net)
{
    DatabaseHandler& handler = *(psn_inst->handler());
    if (k == SteinerNull)
    {
        return;
    }

    if (tree->left(k) == SteinerNull && tree->right(k) == SteinerNull)
    {
        handler.connect(net, tree->pin(k));
    }
    else
    {
        topDownConnect(psn_inst, tree, tree->left(k), net);
        topDownConnect(psn_inst, tree, tree->right(k), net);
    }
}
void
GateCloningTransform::cloneInstance(psn::Psn*                          psn_inst,
                                    std::unique_ptr<psn::SteinerTree>& tree,
                                    psn::SteinerPoint                  k)
{
    DatabaseHandler& handler = *(psn_inst->handler());

    SteinerPoint drvr       = tree->driverPoint();
    auto         output_pin = tree->pin(drvr);
    auto         inst       = handler.instance(output_pin);
    Net*         output_net = handler.net(output_pin);

    std::string clone_net_name = makeUniqueNetName(psn_inst);
    Net*        clone_net      = handler.createNet(clone_net_name.c_str());
    auto        output_port    = handler.libraryPin(output_pin);

    topDownConnect(psn_inst, tree, k, clone_net);

    int fanout_count = handler.fanoutPins(handler.net(output_pin)).size();
    if (fanout_count == 0)
    {
        handler.connect(clone_net, output_pin);
        handler.del(output_net);
    }
    else
    {
        std::string instance_name = makeUniqueCloneName(psn_inst);
        auto        cell          = handler.libraryCell(inst);

        Instance* cloned_inst =
            handler.createInstance(instance_name.c_str(), cell);
        handler.setLocation(cloned_inst, handler.location(output_pin));
        handler.connect(clone_net, cloned_inst, output_port);
        clone_count_++;
        auto pins = handler.pins(inst);
        for (auto& p : pins)
        {
            if (handler.isInput(p) && p != output_pin)
            {
                Net* target_net  = handler.net(p);
                auto target_port = handler.libraryPin(p);
                handler.connect(target_net, cloned_inst, target_port);
            }
        }
    }
}
std::string
GateCloningTransform::makeUniqueNetName(Psn* psn_inst)
{
    DatabaseHandler& handler = *(psn_inst->handler());

    std::string name;
    do
        name = std::string("net_") + std::to_string(net_index_++);
    while (handler.net(name.c_str()));
    return name;
}
std::string
GateCloningTransform::makeUniqueCloneName(Psn* psn_inst)
{
    DatabaseHandler& handler = *(psn_inst->handler());

    std::string name;
    do
        name = std::string("cloned_gate_") + std::to_string(clone_index_++);
    while (handler.instance(name.c_str()));
    return name;
}
bool
GateCloningTransform::isNumber(const std::string& s)
{
    std::istringstream iss(s);
    float              f;
    iss >> std::noskipws >> f;
    return iss.eof() && !iss.fail();
}
int
GateCloningTransform::run(Psn* psn_inst, std::vector<std::string> args)
{
    if (args.size() > 2)
    {
        PsnLogger::instance().error(
            "Usage: transform gate_clone "
            "<float: max-cap-factor> <boolean: clone-gates-only>");
        return -1;
    }
    float cap_factor         = 1.4;
    bool  clone_largest_only = false;
    if (args.size() >= 1)
    {
        if (!isNumber(args[0]))
        {
            PsnLogger::instance().error(
                "Expected number for max-cap-factor, got {}", args[0]);
            return -1;
        }
        cap_factor = std::stof(args[0].c_str());
        if (args.size() >= 2)
        {
            std::transform(args[1].begin(), args[1].end(), args[1].begin(),
                           ::tolower);
            if (args[1] == "true" || args[1] == "1")
            {
                clone_largest_only = true;
            }
            else if (args[1] == "false" || args[1] == "0")
            {
                clone_largest_only = false;
            }
            else
            {
                PsnLogger::instance().error(
                    "Expected boolean for clone-gates-only, got {}", args[0]);
                return -1;
            }
        }
    }
    return gateClone(psn_inst, cap_factor, clone_largest_only);
}
