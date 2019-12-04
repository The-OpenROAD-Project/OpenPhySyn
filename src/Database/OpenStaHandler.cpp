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
#ifdef USE_OPENSTA_DB_HANDLER

// Temproary fix for OpenSTA
#define THROW_DCL throw()

#include <OpenPhySyn/Database/OpenStaHandler.hpp>
#include <OpenPhySyn/PsnLogger/PsnLogger.hpp>
#include <OpenPhySyn/Sta/DatabaseSta.hpp>
#include <OpenPhySyn/Sta/DatabaseStaNetwork.hpp>
#include <OpenSTA/dcalc/DcalcAnalysisPt.hh>
#include <OpenSTA/dcalc/GraphDelayCalc.hh>
#include <OpenSTA/graph/Graph.hh>
#include <OpenSTA/liberty/TimingArc.hh>
#include <OpenSTA/liberty/TimingModel.hh>
#include <OpenSTA/liberty/TimingRole.hh>
#include <OpenSTA/liberty/Transition.hh>
#include <OpenSTA/network/NetworkCmp.hh>
#include <OpenSTA/network/PortDirection.hh>
#include <OpenSTA/sdc/Sdc.hh>
#include <OpenSTA/search/Corner.hh>
#include <OpenSTA/search/Search.hh>
#include <OpenSTA/util/PatternMatch.hh>
#include <algorithm>
#include <set>

namespace psn
{
OpenStaHandler::OpenStaHandler(sta::DatabaseSta* sta)
    : sta_(sta),
      db_(sta->db()),
      has_equiv_cells_(false),
      has_target_loads_(false)
{
    // sta_->findDelays();
    // Use default corner for now
    corner_        = sta_->findCorner("default");
    min_max_       = sta::MinMax::max();
    dcalc_ap_      = corner_->findDcalcAnalysisPt(min_max_);
    pvt_           = dcalc_ap_->operatingConditions();
    parasitics_ap_ = corner_->findParasiticAnalysisPt(min_max_);
}

std::vector<InstanceTerm*>
OpenStaHandler::pins(Net* net) const
{
    std::vector<InstanceTerm*> terms;
    auto                       pin_iter = network()->pinIterator(net);
    while (pin_iter->hasNext())
    {
        InstanceTerm* pin = pin_iter->next();
        terms.push_back(pin);
    }
    return terms;
}
std::vector<InstanceTerm*>
OpenStaHandler::pins(Instance* inst) const
{
    std::vector<InstanceTerm*> terms;
    auto                       pin_iter = network()->pinIterator(inst);
    while (pin_iter->hasNext())
    {
        InstanceTerm* pin = pin_iter->next();
        terms.push_back(pin);
    }
    return terms;
}
Net*
OpenStaHandler::net(InstanceTerm* term) const
{
    return network()->net(term);
}
std::vector<InstanceTerm*>
OpenStaHandler::connectedPins(Net* net) const
{
    std::vector<InstanceTerm*> terms;
    auto                       pin_iter = network()->connectedPinIterator(net);
    while (pin_iter->hasNext())
    {
        InstanceTerm* pin = pin_iter->next();
        terms.push_back(pin);
    }
    std::sort(terms.begin(), terms.end(), sta::PinPathNameLess(network()));
    return terms;
}
std::set<BlockTerm*>
OpenStaHandler::clockPins() const
{
    std::set<BlockTerm*> clock_pins;
    auto                 clk_iter = new sta::ClockIterator(network()->sdc());
    while (clk_iter->hasNext())
    {
        auto clk  = clk_iter->next();
        auto pins = clk->pins();
        for (auto pin : pins)
        {
            clock_pins.insert(pin);
        }
    }
    delete clk_iter;
    return clock_pins;
}

std::vector<InstanceTerm*>
OpenStaHandler::inputPins(Instance* inst) const
{
    auto inst_pins = pins(inst);
    return filterPins(inst_pins, PinDirection::input());
}

std::vector<InstanceTerm*>
OpenStaHandler::outputPins(Instance* inst) const
{
    auto inst_pins = pins(inst);
    return filterPins(inst_pins, PinDirection::output());
}

std::vector<InstanceTerm*>
OpenStaHandler::fanoutPins(Net* net) const
{
    auto inst_pins = pins(net);
    return filterPins(inst_pins, PinDirection::input());
}
std::vector<InstanceTerm*>
OpenStaHandler::levelDriverPins() const
{
    auto                       handler_network = network();
    std::vector<InstanceTerm*> terms;
    std::vector<sta::Vertex*>  vertices;
    sta_->ensureLevelized();
    sta::VertexIterator itr(handler_network->graph());
    while (itr.hasNext())
    {
        sta::Vertex* vtx = itr.next();
        if (vtx->isDriver(handler_network))
            vertices.push_back(vtx);
    }
    std::sort(
        vertices.begin(), vertices.end(),
        [=](const sta::Vertex* v1, const sta::Vertex* v2) -> bool {
            return (v1->level() < v2->level()) ||
                   (v1->level() == v2->level() &&
                    sta::stringLess(handler_network->pathName(v1->pin()),
                                    handler_network->pathName(v2->pin())));
        });
    for (auto& v : vertices)
    {
        terms.push_back(v->pin());
    }
    return terms;
}

InstanceTerm*
OpenStaHandler::faninPin(Net* net) const
{
    auto net_pins = pins(net);
    for (auto& pin : net_pins)
    {
        Instance* inst = network()->instance(pin);
        if (inst)
        {
            if (network()->direction(pin)->isOutput())
            {
                return pin;
            }
        }
    }

    return nullptr;
}

std::vector<Instance*>
OpenStaHandler::fanoutInstances(Net* net) const
{
    std::vector<Instance*>     insts;
    std::vector<InstanceTerm*> net_pins = fanoutPins(net);
    for (auto& term : net_pins)
    {
        insts.push_back(network()->instance(term));
    }
    return insts;
}

std::vector<Instance*>
OpenStaHandler::driverInstances() const
{
    std::set<Instance*> insts_set;
    for (auto& net : nets())
    {
        InstanceTerm* driverPin = faninPin(net);
        if (driverPin)
        {
            Instance* inst = network()->instance(driverPin);
            if (inst)
            {
                insts_set.insert(inst);
            }
        }
    }
    return std::vector<Instance*>(insts_set.begin(), insts_set.end());
}

unsigned int
OpenStaHandler::fanoutCount(Net* net) const
{
    return fanoutPins(net).size();
}

Point
OpenStaHandler::location(InstanceTerm* term)
{
    odb::dbITerm* iterm;
    odb::dbBTerm* bterm;
    network()->staToDb(term, iterm, bterm);
    if (iterm)
    {
        int x, y;
        iterm->getInst()->getOrigin(x, y);
        return Point(x, y);
    }
    if (bterm)
    {
        int x, y;
        if (bterm->getFirstPinLocation(x, y))
            return Point(x, y);
    }
    return Point(0, 0);
}

Point
OpenStaHandler::location(Instance* inst)
{
    odb::dbInst* dinst = network()->staToDb(inst);
    int          x, y;
    dinst->getOrigin(x, y);
    return Point(x, y);
}
void
OpenStaHandler::setLocation(Instance* inst, Point pt)
{
    odb::dbInst* dinst = network()->staToDb(inst);
    dinst->setPlacementStatus(odb::dbPlacementStatus::PLACED);
    dinst->setLocation(pt.getX(), pt.getY());
}
float
OpenStaHandler::area(Instance* inst)
{
    HANDLER_UNSUPPORTED_METHOD(OpenStaHandler, area)
    return 0;
}
float
OpenStaHandler::area()
{
    HANDLER_UNSUPPORTED_METHOD(OpenStaHandler, area)
    return 0;
}
LibraryTerm*
OpenStaHandler::libraryPin(InstanceTerm* term) const
{
    return network()->libertyPort(term);
}
bool
OpenStaHandler::isClocked(InstanceTerm* term) const
{
    sta::Vertex *vertex, *bidirect_drvr_vertex;
    sta_->graph()->pinVertices(term, vertex, bidirect_drvr_vertex);
    return sta_->search()->isClock(vertex);
}
bool
OpenStaHandler::isPrimary(Net* net) const
{
    for (auto& pin : pins(net))
    {
        if (network()->isTopLevelPort(pin))
        {
            return true;
        }
    }
    return false;
}

LibraryCell*
OpenStaHandler::libraryCell(InstanceTerm* term) const
{
    auto inst = network()->instance(term);
    if (inst)
    {
        return network()->libertyCell(inst);
    }
    return nullptr;
}

LibraryCell*
OpenStaHandler::libraryCell(Instance* inst) const
{
    return network()->libertyCell(inst);
}

LibraryCell*
OpenStaHandler::libraryCell(const char* name) const
{
    return network()->findLibertyCell(name);
}
LibraryCell*
OpenStaHandler::largestLibraryCell(LibraryCell* cell)
{
    if (!has_equiv_cells_)
    {
        makeEquivalentCells();
    }
    auto  equiv_cells = sta_->equivCells(cell);
    auto  largest     = cell;
    float current_max = maxLoad(cell);
    if (equiv_cells)
    {
        for (auto e_cell : *equiv_cells)
        {
            auto cell_load = maxLoad(e_cell);
            if (cell_load > current_max)
            {
                current_max = cell_load;
                largest     = e_cell;
            }
        }
    }
    return largest;
}
double
OpenStaHandler::dbuToMeters(uint dist) const
{
    return dist * 1E-9;
}
bool
OpenStaHandler::isPlaced(InstanceTerm* term) const
{
    odb::dbITerm* iterm;
    odb::dbBTerm* bterm;
    network()->staToDb(term, iterm, bterm);
    odb::dbPlacementStatus status = odb::dbPlacementStatus::UNPLACED;
    if (iterm)
    {
        odb::dbInst* inst = iterm->getInst();
        status            = inst->getPlacementStatus();
    }
    if (bterm)
        status = bterm->getFirstPinPlacementStatus();
    return status == odb::dbPlacementStatus::PLACED ||
           status == odb::dbPlacementStatus::LOCKED ||
           status == odb::dbPlacementStatus::FIRM ||
           status == odb::dbPlacementStatus::COVER;
}
bool
OpenStaHandler::isPlaced(Instance* inst) const
{
    odb::dbInst*           dinst  = network()->staToDb(inst);
    odb::dbPlacementStatus status = dinst->getPlacementStatus();
    return status == odb::dbPlacementStatus::PLACED ||
           status == odb::dbPlacementStatus::LOCKED ||
           status == odb::dbPlacementStatus::FIRM ||
           status == odb::dbPlacementStatus::COVER;
}
bool
OpenStaHandler::isDriver(InstanceTerm* term) const
{
    return network()->isDriver(term);
}

float
OpenStaHandler::pinCapacitance(InstanceTerm* term) const
{
    auto port = network()->libertyPort(term);
    if (port)
    {
        return pinCapacitance(port);
    }
    return 0.0;
}
float
OpenStaHandler::pinCapacitance(LibraryTerm* term) const
{
    float cap1 = term->capacitance(sta::RiseFall::rise(), min_max_);
    float cap2 = term->capacitance(sta::RiseFall::fall(), min_max_);
    return std::max(cap1, cap2);
}

Instance*
OpenStaHandler::instance(const char* name) const
{
    return network()->findInstance(name);
}
Instance*
OpenStaHandler::instance(InstanceTerm* term) const
{
    return network()->instance(term);
}
Net*
OpenStaHandler::net(const char* name) const
{
    return network()->findNet(name);
}

LibraryTerm*
OpenStaHandler::libraryPin(const char* cell_name, const char* pin_name) const
{
    LibraryCell* cell = libraryCell(cell_name);
    if (!cell)
    {
        return nullptr;
    }
    return libraryPin(cell, pin_name);
}
LibraryTerm*
OpenStaHandler::libraryPin(LibraryCell* cell, const char* pin_name) const
{
    return cell->findLibertyPort(pin_name);
}

std::vector<InstanceTerm*>
OpenStaHandler::filterPins(std::vector<InstanceTerm*>& terms,
                           PinDirection*               direction) const
{
    std::vector<InstanceTerm*> inst_terms;
    for (auto& term : terms)
    {
        Instance* inst = network()->instance(term);
        if (inst)
        {
            if (term && network()->direction(term) == direction)
            {
                inst_terms.push_back(term);
            }
        }
    }
    return inst_terms;
}
void
OpenStaHandler::del(Net* net) const
{
    network()->deleteNet(net);
}
int
OpenStaHandler::disconnectAll(Net* net) const
{
    int count = 0;
    for (auto& pin : pins(net))
    {
        network()->disconnectPin(pin);
        count++;
    }
    return count;
}

void
OpenStaHandler::connect(Net* net, InstanceTerm* term) const
{
    auto inst      = network()->instance(term);
    auto term_port = network()->port(term);
    network()->connect(inst, term_port, net);
}

void
OpenStaHandler::disconnect(InstanceTerm* term) const
{
    network()->disconnectPin(term);
}

Instance*
OpenStaHandler::createInstance(const char* inst_name, LibraryCell* cell)
{
    return network()->makeInstance(cell, inst_name, network()->topInstance());
}

Net*
OpenStaHandler::createNet(const char* net_name)
{
    return network()->makeNet(net_name, network()->topInstance());
}

InstanceTerm*
OpenStaHandler::connect(Net* net, Instance* inst, LibraryTerm* port) const
{
    return network()->connect(inst, port, net);
}

std::vector<Net*>
OpenStaHandler::nets() const
{
    sta::NetSeq       all_nets;
    sta::PatternMatch pattern("*");
    network()->findNetsMatching(network()->topInstance(), &pattern, &all_nets);
    return static_cast<std::vector<Net*>>(all_nets);
}

std::string
OpenStaHandler::topName() const
{
    return std::string(network()->name(network()->topInstance()));
}
std::string
OpenStaHandler::name(Block* object) const
{
    return std::string(object->getConstName());
}
std::string
OpenStaHandler::name(Net* object) const
{
    return std::string(network()->name(object));
}
std::string
OpenStaHandler::name(Instance* object) const
{
    return std::string(network()->name(object));
}
std::string
OpenStaHandler::name(BlockTerm* object) const
{
    return std::string(network()->name(object));
}
std::string
OpenStaHandler::name(Library* object) const
{
    return std::string(object->getConstName());
}
std::string
OpenStaHandler::name(LibraryCell* object) const
{
    return std::string(network()->name(network()->cell(object)));
}
std::string
OpenStaHandler::name(LibraryTerm* object) const
{
    return object->name();
}

Library*
OpenStaHandler::library() const
{
    LibrarySet libs = db_->getLibs();

    if (!libs.size())
    {
        return nullptr;
    }
    Library* lib = *(libs.begin());
    return lib;
}

LibraryTechnology*
OpenStaHandler::technology() const
{
    Library* lib = library();
    if (!lib)
    {
        return nullptr;
    }
    LibraryTechnology* tech = lib->getTech();
    return tech;
}

Block*
OpenStaHandler::top() const
{
    Chip* chip = db_->getChip();
    if (!chip)
    {
        return nullptr;
    }

    Block* block = chip->getBlock();
    return block;
}

OpenStaHandler::~OpenStaHandler()
{
}
void
OpenStaHandler::clear() const
{
    db_->clear();
}
sta::DatabaseStaNetwork*
OpenStaHandler::network() const
{
    return sta_->getDbNetwork();
}
sta::DatabaseSta*
OpenStaHandler::sta() const
{
    return sta_;
}
float
OpenStaHandler::maxLoad(LibraryCell* cell)
{
    sta::LibertyCellPortIterator itr(cell);
    while (itr.hasNext())
    {
        auto port = itr.next();
        if (port->direction() == PinDirection::output())
        {
            float limit;
            bool  exists;
            port->capacitanceLimit(min_max_, limit, exists);
            if (exists)
            {
                return limit;
            }
        }
    }
    return 0;
}
float
OpenStaHandler::maxLoad(LibraryTerm* term)
{
    float limit;
    bool  exists;
    term->capacitanceLimit(min_max_, limit, exists);
    if (exists)
    {
        return limit;
    }
    return 0;
}
bool
OpenStaHandler::isInput(InstanceTerm* term) const
{
    return network()->direction(term)->isInput();
}
bool
OpenStaHandler::isOutput(InstanceTerm* term) const
{
    return network()->direction(term)->isOutput();
}
bool
OpenStaHandler::isAnyInput(InstanceTerm* term) const
{
    return network()->direction(term)->isAnyInput();
}
bool
OpenStaHandler::isAnyOutput(InstanceTerm* term) const
{
    return network()->direction(term)->isAnyOutput();
}
bool
OpenStaHandler::isBiDriect(InstanceTerm* term) const
{
    return network()->direction(term)->isBidirect();
}
bool
OpenStaHandler::isTriState(InstanceTerm* term) const
{
    return network()->direction(term)->isTristate();
}
bool
OpenStaHandler::isInput(LibraryTerm* term) const
{
    return term->direction()->isInput();
}
bool
OpenStaHandler::isOutput(LibraryTerm* term) const
{
    return term->direction()->isOutput();
}
bool
OpenStaHandler::isAnyInput(LibraryTerm* term) const
{
    return term->direction()->isAnyInput();
}
bool
OpenStaHandler::isAnyOutput(LibraryTerm* term) const
{
    return term->direction()->isAnyOutput();
}
bool
OpenStaHandler::isBiDriect(LibraryTerm* term) const
{
    return term->direction()->isBidirect();
}
bool
OpenStaHandler::isTriState(LibraryTerm* term) const
{
    return term->direction()->isTristate();
}
bool
OpenStaHandler::hasMaxCapViolation(InstanceTerm* term) const
{
    LibraryTerm* port = network()->libertyPort(term);
    if (port)
    {
        float load_cap = network()->graphDelayCalc()->loadCap(term, dcalc_ap_);
        float cap_limit;
        bool  exists;
        port->capacitanceLimit(sta::MinMax::max(), cap_limit, exists);
        return exists && load_cap > cap_limit;
    }
    return false;
}
void
OpenStaHandler::makeEquivalentCells()
{
    sta::LibertyLibrarySeq       map_libs;
    sta::LibertyLibraryIterator* lib_iter = network()->libertyLibraryIterator();
    while (lib_iter->hasNext())
    {
        auto lib = lib_iter->next();
        map_libs.push_back(lib);
    }
    delete lib_iter;
    auto all_libs = allLibs();
    sta_->makeEquivCells(&all_libs, &map_libs);
    has_equiv_cells_ = true;
}
sta::LibertyLibrarySeq
OpenStaHandler::allLibs() const
{
    sta::LibertyLibrarySeq       seq;
    sta::LibertyLibraryIterator* iter = network()->libertyLibraryIterator();
    while (iter->hasNext())
    {
        sta::LibertyLibrary* lib = iter->next();
        seq.push_back(lib);
    }
    delete iter;
    return seq;
}
void
OpenStaHandler::resetCache()
{
    has_equiv_cells_  = false;
    has_target_loads_ = false;
    target_load_map_.clear();
}
void
OpenStaHandler::findTargetLoads()
{
    auto all_libs = allLibs();
    findTargetLoads(&all_libs);
    has_target_loads_ = true;
}

/* The following is borrowed from James Cherry's Resizer Code */

// Find a target slew for the libraries and then
// a target load for each cell that gives the target slew.
void
OpenStaHandler::findTargetLoads(sta::LibertyLibrarySeq* resize_libs)
{
    // Find target slew across all buffers in the libraries.
    findBufferTargetSlews(resize_libs);
    for (auto lib : *resize_libs)
        findTargetLoads(lib, target_slews_);
}

float
OpenStaHandler::targetLoad(LibraryCell* cell)
{
    if (!has_target_loads_)
    {
        findTargetLoads();
    }
    if (target_load_map_.count(cell))
    {
        return target_load_map_[cell];
    }
    return 0.0;
}

void
OpenStaHandler::findTargetLoads(Liberty* library, sta::Slew slews[])
{
    sta::LibertyCellIterator cell_iter(library);
    while (cell_iter.hasNext())
    {
        auto cell = cell_iter.next();
        findTargetLoad(cell, slews);
    }
}

void
OpenStaHandler::findTargetLoad(LibraryCell* cell, sta::Slew slews[])
{
    sta::LibertyCellTimingArcSetIterator arc_set_iter(cell);
    float                                target_load_sum = 0.0;
    int                                  arc_count       = 0;
    while (arc_set_iter.hasNext())
    {
        auto arc_set = arc_set_iter.next();
        auto role    = arc_set->role();
        if (!role->isTimingCheck() &&
            role != sta::TimingRole::tristateDisable() &&
            role != sta::TimingRole::tristateEnable())
        {
            sta::TimingArcSetArcIterator arc_iter(arc_set);
            while (arc_iter.hasNext())
            {
                sta::TimingArc* arc    = arc_iter.next();
                sta::RiseFall*  in_rf  = arc->fromTrans()->asRiseFall();
                sta::RiseFall*  out_rf = arc->toTrans()->asRiseFall();
                float           arc_target_load = findTargetLoad(
                    cell, arc, slews[in_rf->index()], slews[out_rf->index()]);
                target_load_sum += arc_target_load;
                arc_count++;
            }
        }
    }
    float target_load = (arc_count > 0) ? target_load_sum / arc_count : 0.0;
    target_load_map_[cell] = target_load;
}

// Find the load capacitance that will cause the output slew
// to be equal to out_slew.
float
OpenStaHandler::findTargetLoad(LibraryCell* cell, sta::TimingArc* arc,
                               sta::Slew in_slew, sta::Slew out_slew)
{
    sta::GateTimingModel* model =
        dynamic_cast<sta::GateTimingModel*>(arc->model());
    if (model)
    {
        float cap_init = 1.0e-12;         // 1pF
        float cap_tol  = cap_init * .001; // .1%
        float load_cap = cap_init;
        float cap_step = cap_init;
        while (cap_step > cap_tol)
        {
            sta::ArcDelay arc_delay;
            sta::Slew     arc_slew;
            model->gateDelay(cell, pvt_, in_slew, load_cap, 0.0, false,
                             arc_delay, arc_slew);
            if (arc_slew > out_slew)
            {
                load_cap -= cap_step;
                cap_step /= 2.0;
            }
            load_cap += cap_step;
        }
        return load_cap;
    }
    return 0.0;
}

////////////////////////////////////////////////////////////////

sta::Slew
OpenStaHandler::targetSlew(const sta::RiseFall* rf)
{
    return target_slews_[rf->index()];
}

// Find target slew across all buffers in the libraries.
void
OpenStaHandler::findBufferTargetSlews(sta::LibertyLibrarySeq* resize_libs)
{
    target_slews_[sta::RiseFall::riseIndex()] = 0.0;
    target_slews_[sta::RiseFall::fallIndex()] = 0.0;
    int tgt_counts[sta::RiseFall::index_count]{0};

    for (auto lib : *resize_libs)
    {
        sta::Slew slews[sta::RiseFall::index_count]{0.0};
        int       counts[sta::RiseFall::index_count]{0};

        findBufferTargetSlews(lib, slews, counts);
        for (auto rf : sta::RiseFall::rangeIndex())
        {
            target_slews_[rf] += slews[rf];
            tgt_counts[rf] += counts[rf];
            slews[rf] /= counts[rf];
        }
    }

    for (auto rf : sta::RiseFall::rangeIndex())
        target_slews_[rf] /= tgt_counts[rf];
}
bool
OpenStaHandler::dontUse(LibraryCell* cell) const
{
    return false;
}
void
OpenStaHandler::findBufferTargetSlews(Liberty* library,
                                      // Return values.
                                      sta::Slew slews[], int counts[])
{
    for (auto buffer : *library->buffers())
    {
        if (!dontUse(buffer))
        {
            sta::LibertyPort *input, *output;
            buffer->bufferPorts(input, output);
            auto arc_sets = buffer->timingArcSets(input, output);
            if (arc_sets)
            {
                for (auto arc_set : *arc_sets)
                {
                    sta::TimingArcSetArcIterator arc_iter(arc_set);
                    while (arc_iter.hasNext())
                    {
                        sta::TimingArc*       arc = arc_iter.next();
                        sta::GateTimingModel* model =
                            dynamic_cast<sta::GateTimingModel*>(arc->model());
                        sta::RiseFall* in_rf  = arc->fromTrans()->asRiseFall();
                        sta::RiseFall* out_rf = arc->toTrans()->asRiseFall();
                        float in_cap   = input->capacitance(in_rf, min_max_);
                        float load_cap = in_cap * 10.0; // "factor debatable"
                        sta::ArcDelay arc_delay;
                        sta::Slew     arc_slew;
                        model->gateDelay(buffer, pvt_, 0.0, load_cap, 0.0,
                                         false, arc_delay, arc_slew);
                        model->gateDelay(buffer, pvt_, arc_slew, load_cap, 0.0,
                                         false, arc_delay, arc_slew);
                        slews[out_rf->index()] += arc_slew;
                        counts[out_rf->index()]++;
                    }
                }
            }
        }
    }
}
} // namespace psn
#endif