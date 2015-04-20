// Copyright 2009-2015 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2015, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#include "sst_config.h"
#include "sst/core/serialization.h"
#include <sst/core/simulation.h>

#include <utility>

#include <boost/foreach.hpp>

//#include <sst/core/archive.h>
#include <sst/core/clock.h>
#include <sst/core/config.h>
#include <sst/core/configGraph.h>
#include <sst/core/heartbeat.h>
#include <sst/core/debug.h>
//#include <sst/core/event.h>
#include <sst/core/exit.h>
#include <sst/core/factory.h>
//#include <sst/core/graph.h>
#include <sst/core/introspector.h>
#include <sst/core/linkMap.h>
#include <sst/core/linkPair.h>
#include <sst/core/output.h>
#include <sst/core/stopAction.h>
#include <sst/core/sync.h>
#include <sst/core/syncQueue.h>
#include <sst/core/timeLord.h>
#include <sst/core/timeVortex.h>
#include <sst/core/unitAlgebra.h>

#define SST_SIMTIME_MAX  0xffffffffffffffff

using namespace SST::Statistics;

namespace SST { 

SimulationBase::SimulationBase(Config *config)
{
    factory = new Factory(config->getLibPath());
    timeLord = new TimeLord(config->timeBase);
    
    // Create the Statistic Processing Engine
    statisticsEngine = new StatisticProcessingEngine();
    statisticsOutput = NULL; 
}

SimulationBase::~SimulationBase()
{
    //    delete factory;
    delete timeLord;
    
    // Delete the Statistic Objects
    if (NULL != statisticsOutput) {
        delete statisticsOutput;
    }
    delete statisticsEngine;
}

TimeConverter* SimulationBase::minPartToTC(SimTime_t cycles) const {
    return timeLord->getTimeConverter(cycles);
}

template<class Archive>
void
SimulationBase::serialize(Archive & ar, const unsigned int version)
{
    printf("begin SimulationBase::serialize\n");
    printf("  - SimulationBase::factory\n");
    ar & BOOST_SERIALIZATION_NVP(factory);
    printf("  - SimulationBase::timeLord\n");
    ar & BOOST_SERIALIZATION_NVP(timeLord);
    printf("  - SimulationBase::statisticsOutput\n");
    ar & BOOST_SERIALIZATION_NVP(statisticsOutput);
    printf("  - SimulationBase::statisticsEngine\n");
    ar & BOOST_SERIALIZATION_NVP(statisticsEngine);
    printf("end SimulationBase::serialize\n");
}

Simulation* Simulation::instance = NULL;

Simulation::~Simulation()
{
    // Clean up as best we can

    // Delete the timeVortex first.  This will delete all events left
    // in the queue, as well as the Sync, Exit and Clock objects.
    delete timeVortex;

    // Delete all the components
    for ( CompMap_t::iterator it = compMap.begin(); it != compMap.end(); ++it ) {
	delete it->second;
    }
    compMap.clear();
    
    // Delete all the introspectors
    for ( IntroMap_t::iterator it = introMap.begin(); it != introMap.end(); ++it ) {
	delete it->second;
    }
    introMap.clear();

    // Clocks already got deleted by timeVortex, simply clear the clockMap
    clockMap.clear();
    
    // OneShots already got deleted by timeVortex, simply clear the onsShotMap
    oneShotMap.clear();
    
    // // Delete any remaining links.  This should never happen now, but
    // // when we add an API to have components build subcomponents, user
    // // error could cause LinkMaps to be left.
    // std::map<ComponentId_t,LinkMap*>::iterator it;
    // for ( it = component_links.begin(); it != component_links.end(); ++it ) {
    //     delete it->second;
    // }
    // component_links.clear();
}

Simulation*
Simulation::createSimulation(Config *config, int my_rank, int num_ranks)
{
    instance = new Simulation(config,my_rank,num_ranks);
    return instance;
}


Simulation::Simulation( Config* cfg, int my_rank, int num_ranks ) :
    SimulationBase(cfg),
    runMode(cfg->runMode),
    timeVortex(NULL),
    minPartTC( NULL ),
    sync(NULL),
    currentSimCycle(0),
    endSimCycle(0),
    currentPriority(0),
    endSim(false),
    my_rank(my_rank),
    num_ranks(num_ranks),
    init_msg_count(0),
    init_phase(0),
    lastRecvdSignal(0)
{
//     eQueue = new EventQueue_t;
    sim_output.init("SSTCore", cfg->getVerboseLevel(), 0, Output::STDOUT);
    output_directory = "";

    timeVortex = new TimeVortex;
    m_exit = new Exit( this, timeLord->getTimeConverter("100ns"), num_ranks == 1 );

    if(strcmp(cfg->heartbeatPeriod.c_str(), "N") != 0) {
        sim_output.output("# Creating simulation heartbeat at period of %s.\n", cfg->heartbeatPeriod.c_str());
    	m_heartbeat = new SimulatorHeartbeat(cfg, my_rank, this, timeLord->getTimeConverter(cfg->heartbeatPeriod) );
    }
}

void
Simulation::setStopAtCycle( Config* cfg )
{
    SimTime_t stopAt = timeLord->getSimCycles(cfg->stopAtCycle,"StopAction configure");
    if ( stopAt != 0 ) {
	printf("Inserting stop event at cycle %s, %" PRIu64 "\n",
	       cfg->stopAtCycle.c_str(), stopAt);
	StopAction* sa = new StopAction();
	sa->setDeliveryTime(stopAt);
	timeVortex->insert(sa);
    }
}

Simulation::Simulation()
{
    // serialization only, so everything will be restored.  I think.
}


/** If sig == -1, be quite about shutdown */
void Simulation::emergencyShutdown(int sig)
{
    if ( sig != -1 ) {
        sim_output.output("EMERGENCY SHUTDOWN!\n");
        sim_output.output("# Simulated time:                  %s\n", getElapsedSimTime().toStringBestSI().c_str());

        signal(sig, SIG_DFL); // Restore default handler
    }

    if ( sig == SIGINT ) {
        for ( CompMap_t::iterator iter = compMap.begin(); iter != compMap.end(); ++iter ) {
            (*iter).second->finish();
        }
    }

    for ( CompMap_t::iterator iter = compMap.begin(); iter != compMap.end(); ++iter ) {
        (*iter).second->emergencyShutdown();
    }

    if ( sig != -1 ) {
        sim_output.output("EMERGENCY SHUTDOWN COMPLETE!\n");
        sim_output.output("# Simulated time:                  %s\n", getElapsedSimTime().toStringBestSI().c_str());
    }

    exit(1);
}


Component*
Simulation::createComponent( ComponentId_t id, std::string name, 
                             Params params )
{
    return factory->CreateComponent(id, name, params);
}

Introspector*
Simulation::createIntrospector(std::string name, Params params )
{
    return factory->CreateIntrospector(name, params);
}

void
Simulation::requireEvent(std::string name)
{
    factory->RequireEvent(name);
}
    
SimTime_t
Simulation::getNextActivityTime()
{
    return timeVortex->front()->getDeliveryTime();
}
    
int Simulation::performWireUp( ConfigGraph& graph, int myRank, SimTime_t min_part )
{
    // Create the Statistics Output
    statisticsOutput = factory->CreateStatisticOutput(graph.getStatOutput(), graph.getStatOutputParams());
    if (NULL == statisticsOutput) {
        sim_output.fatal(CALL_INFO, -1, " - Unable to instantiate Statistic Output %s\n", graph.getStatOutput().c_str());
    }
    
    if (false == statisticsOutput->checkOutputParameters()) {
        // If checkOutputParameters() fail, Tell the user how to use them and abort simulation
        sim_output.output("Statistic Output (%s) :\n", statisticsOutput->getStatisticOutputName().c_str());
        statisticsOutput->printUsage();
        sim_output.output("\n");

        sim_output.output("Statistic Output Parameters Provided:\n");
        for (Params::iterator it = graph.getStatOutputParams().begin(); it != graph.getStatOutputParams().end(); ++it ) {
            printf("  %s = %s\n", Params::getParamName(it->first).c_str(), it->second.c_str());
        }
        sim_output.fatal(CALL_INFO, -1, " - Required Statistic Output Parameters not set\n");
    }
    
    // Set the Statistics Load Level into the Statistic Output 
    statisticsOutput->setStatisticLoadLevel(graph.getStatLoadLevel());
    
    // Params objects should now start verifying parameters
    Params::enableVerify();

    if ( num_ranks > 1 ) {
        sync = new SyncD();
        sync->setExit(m_exit);
        sync->setMaxPeriod( minPartTC = minPartToTC(min_part) );
    }

    // First, go through all the components that are in this rank and
    // create the ComponentInfo object for it
    // Now, build all the components
    for ( ConfigComponentMap_t::iterator iter = graph.comps.begin();
            iter != graph.comps.end(); ++iter )
    {
        ConfigComponent* ccomp = &(*iter);
        if ( ccomp->rank == myRank ) {
            compInfoMap[ccomp->id] = ComponentInfo(ccomp->name, ccomp->type, new LinkMap());
        }
    }
    
    
    // We will go through all the links and create LinkPairs for each
    // link.  We will also create a LinkMap for each component and put
    // them into a map with ComponentID as the key.
    for( ConfigLinkMap_t::iterator iter = graph.links.begin();
            iter != graph.links.end(); ++iter )
    {
        ConfigLink &clink = *iter;
        int rank[2];
        rank[0] = graph.comps[clink.component[0]].rank;
        rank[1] = graph.comps[clink.component[1]].rank;

        if ( rank[0] != myRank && rank[1] != myRank ) {
            // Nothing to be done
            continue;
        }
        else if ( rank[0] == rank[1] ) {
            // Create a LinkPair to represent this link
            LinkPair lp(clink.id);

            lp.getLeft()->setLatency(clink.latency[0]);
            lp.getRight()->setLatency(clink.latency[1]);

            // Add this link to the appropriate LinkMap
            CompInfoMap_t::iterator it;
            // it = component_links.find(clink.component[0]);
            it = compInfoMap.find(clink.component[0]);
            if ( it == compInfoMap.end() ) {
                // This shouldn't happen and is an error
                sim_output.fatal(CALL_INFO,1,"Couldn't find ComponentInfo in map.");
            }
            it->second.link_map->insertLink(clink.port[0],lp.getLeft());

            // it = component_links.find(clink.component[1]->id);
            it = compInfoMap.find(clink.component[1]);
            if ( it == compInfoMap.end() ) {
                // This shouldn't happen and is an error
                sim_output.fatal(CALL_INFO,1,"Couldn't find ComponentInfo in map.");
            }
            it->second.link_map->insertLink(clink.port[1],lp.getRight());

        }
        else {
            int local, remote;
            if ( rank[0] == myRank ) {
                local = 0;
                remote = 1;
            }
            else {
                local = 1;
                remote = 0;
            }

            // Create a LinkPair to represent this link
            LinkPair lp(clink.id);

            lp.getLeft()->setLatency(clink.latency[local]);
            lp.getRight()->setLatency(0);
            lp.getRight()->setDefaultTimeBase(minPartToTC(1));


            // Add this link to the appropriate LinkMap for the local component
            CompInfoMap_t::iterator it;
            it = compInfoMap.find(clink.component[local]);
            if ( it == compInfoMap.end() ) {
                // This shouldn't happen and is an error
                sim_output.fatal(CALL_INFO,1,"Couldn't find ComponentInfo in map.");
            }
            it->second.link_map->insertLink(clink.port[local],lp.getLeft());

            // For the remote side, register with sync object
            ActivityQueue* sync_q = sync->registerLink(rank[remote],clink.id,lp.getRight());
            // lp.getRight()->recvQueue = sync_q;
            lp.getRight()->configuredQueue = sync_q;
            lp.getRight()->initQueue = sync_q;
        }

    }
    // Done with that edge, delete it.
    graph.links.clear();

    // Now, build all the components
    for ( ConfigComponentMap_t::iterator iter = graph.comps.begin();
            iter != graph.comps.end(); ++iter )
    {
        ConfigComponent* ccomp = &(*iter);

        if (ccomp->isIntrospector) {
            Introspector* tmp;

            // _SIM_DBG("creating introspector: name=\"%s\" type=\"%s\" id=%d\n",
            // 	     name.c_str(), sdl_c->type().c_str(), (int)id );

            tmp = createIntrospector( ccomp->type, ccomp->params );
            introMap[ccomp->name] = tmp;
        }
        else if ( ccomp->rank == myRank ) {
            Component* tmp;
            //             _SIM_DBG("creating component: name=\"%s\" type=\"%s\" id=%d\n",
            // 		     name.c_str(), sdl_c->type().c_str(), (int)id );

            // Check to make sure there are any entries in the component's LinkMap
            if ( compInfoMap[ccomp->id].link_map->empty() ) {
                printf("WARNING: Building component \"%s\" with no links assigned.\n",ccomp->name.c_str());
            }

            // Save off what statistics can be enabled before instantiating the component
            // This allows the component to register its statistics in its constructor.
            statisticEnableMap[ccomp->id] = &(ccomp->enabledStatistics);
            statisticParamsMap[ccomp->id] = &(ccomp->enabledStatParams);
            
            // compIdMap[ccomp->id] = ccomp->name;
            tmp = createComponent( ccomp->id, ccomp->type,
                    ccomp->params );
            compMap[ccomp->name] = tmp;
            
            // After component is created, clear out the statisticEnableMap so 
            // we dont eat up lots of memory
            statisticEnableMap.erase(ccomp->id);
            statisticParamsMap.erase(ccomp->id);
        }
    } // end for all vertex    
    // Done with verticies, delete them;
    graph.comps.clear();
    statisticEnableMap.clear();
    statisticParamsMap.clear();
    return 0;
}

void Simulation::initialize() {
    bool done = false;
    do {
	init_msg_count = 0;
    	for ( CompMap_t::iterator iter = compMap.begin(); iter != compMap.end(); ++iter ) {
    	    (*iter).second->init(init_phase);
    	}

	// Exchange data for parallel jobs
	if ( num_ranks > 1 ) {
	    init_msg_count = sync->exchangeLinkInitData(init_msg_count);
	}

	// We're done if no new messages were sent
	if ( init_msg_count == 0 ) done = true;

	init_phase++;
    } while ( !done);

    // Walk through all the links and call finalizeConfiguration
    for ( CompInfoMap_t::iterator i = compInfoMap.begin(); i != compInfoMap.end(); ++i) {
        std::map<std::string,Link*>& map = (*i).second.link_map->getLinkMap();
        for ( std::map<std::string,Link*>::iterator j = map.begin(); j != map.end(); ++j ) {
            (*j).second->finalizeConfiguration();
        }
    }
    if ( num_ranks > 1 ) {
	sync->finalizeLinkConfigurations();
    }
}

void Simulation::run() {  
    _SIM_DBG( "RUN\n" );

    // Output tmp_debug("@r: @t:  ",5,-1,Output::FILE);
    
    for( CompMap_t::iterator iter = compMap.begin();
                            iter != compMap.end(); ++iter )
    {
      (*iter).second->setup();
    }
    //for introspector
    for( IntroMap_t::iterator iter = introMap.begin();
                            iter != introMap.end(); ++iter )
    {
      (*iter).second->setup();
    }

    // Fix taken from Scott's change to mainline (SDH, Sep 27 12)
    // Put a stop event at the end of the timeVortex. Simulation will
    // only get to this is there are no other events in the queue.
    // In general, this shouldn't happen, especially for parallel
    // simulations. If it happens in a parallel simulation the 
    // simulation will likely deadlock as only some of the ranks
    // will hit the anomoly.
    StopAction* sa = new StopAction("*** Event queue empty, exiting simulation... ***");
    sa->setDeliveryTime(SST_SIMTIME_MAX);
    timeVortex->insert(sa);

    // Tell the Statistics Output that the simulation is starting
    statisticsOutput->startOfSimulation();
    
    while( LIKELY( ! endSim ) ) {
        if ( UNLIKELY( 0 != lastRecvdSignal ) ) {
            printStatus(lastRecvdSignal == SIGUSR2);
            lastRecvdSignal = 0;
        }
        currentSimCycle = timeVortex->front()->getDeliveryTime();
        currentPriority = timeVortex->front()->getPriority();
        current_activity = timeVortex->pop();
        // current_activity->print("",tmp_debug);
        current_activity->execute();
    }

    // Tell the Statistics Engine and Output that the simulation is finished 
    statisticsEngine->endOfSimulation();
    statisticsOutput->endOfSimulation();
    
    for( CompMap_t::iterator iter = compMap.begin();
                            iter != compMap.end(); ++iter )
    {
      (*iter).second->finish();
    }
    //for introspector
    for( IntroMap_t::iterator iter = introMap.begin();
                            iter != introMap.end(); ++iter )
    {
      (*iter).second->finish();
    }

}

SimTime_t
Simulation::getCurrentSimCycle() const
{
    return currentSimCycle; 
}

SimTime_t
Simulation::getEndSimCycle() const
{
    return endSimCycle; 
}

int
Simulation::getCurrentPriority() const
{
    return currentPriority; 
}


// void Simulation::getElapsedSimTime(double *value, char *prefix) const
// {
//     static const char siprefix[] = {
//         'a', 'f', 'p', 'n', 'u', 'm', ' '
//     };
//     const int max = sizeof(siprefix) / sizeof(char);

//     double val = getCurrentSimCycle() * 1000;
//     int lvl = 0;
//     while ( (lvl < max) && (val > 1000.0) ) {
//         val /= 1000.0;
//         ++lvl;
//     }
//     *value = val;
//     *prefix = siprefix[lvl];

// }

UnitAlgebra Simulation::getElapsedSimTime() const
{
    return timeLord->getTimeBase() * getCurrentSimCycle();
}

UnitAlgebra Simulation::getFinalSimTime() const
{
    return timeLord->getTimeBase() * getEndSimCycle();
}

void Simulation::setSignal(int signal)
{
    switch ( signal ) {
    case SIGINT:
    case SIGTERM:
        instance->emergencyShutdown(signal);
        break;
    default:
        instance->lastRecvdSignal = signal;
        break;
    }
}

void Simulation::printStatus(bool fullStatus)
{
    Output out("SimStatus: @R:@t:", 0, 0, Output::STDERR);
    out.output("\tCurrentSimCycle:  %" PRIu64 "\n", currentSimCycle);

    if ( fullStatus ) {
        timeVortex->print(out);
        out.output("---- Components: ----\n");
        for (CompMap_t::iterator iter = compMap.begin() ; iter != compMap.end() ; ++iter ) {
            iter->second->printStatus(out);
        }
    }


}

TimeConverter* Simulation::registerClock( std::string freq, Clock::HandlerBase* handler )
{
//     _SIM_DBG("freq=%f handler=%p\n", frequency, handler );
    
    TimeConverter* tcFreq = timeLord->getTimeConverter(freq);

    if ( clockMap.find( tcFreq->getFactor() ) == clockMap.end() ) {
        _SIM_DBG( "\n" );
        Clock* ce = new Clock( tcFreq );
        clockMap[ tcFreq->getFactor() ] = ce; 
        
        // ce->setDeliveryTime( currentSimCycle + tcFreq->getFactor() );
        // timeVortex->insert( ce );
        ce->schedule();
    }
    clockMap[ tcFreq->getFactor() ]->registerHandler( handler );
    return tcFreq;    
}

TimeConverter* Simulation::registerClock(const UnitAlgebra& freq, Clock::HandlerBase* handler)
{
    TimeConverter* tcFreq = timeLord->getTimeConverter(freq);
    
    if ( clockMap.find( tcFreq->getFactor() ) == clockMap.end() ) {
        Clock* ce = new Clock( tcFreq );
        clockMap[ tcFreq->getFactor() ] = ce; 

        ce->schedule();
    }
    clockMap[ tcFreq->getFactor() ]->registerHandler( handler );
    return tcFreq;
}

Cycle_t Simulation::reregisterClock( TimeConverter* tc, Clock::HandlerBase* handler )
{
    if ( clockMap.find( tc->getFactor() ) == clockMap.end() ) {
        Output out("Simulation: @R:@t:", 0, 0, Output::STDERR);
        out.fatal(CALL_INFO, 1, "Tried to reregister with a clock that was not previously registered, exiting...\n");
    }
    clockMap[ tc->getFactor() ]->registerHandler( handler );
    return clockMap[tc->getFactor()]->getNextCycle();
    
}

Cycle_t Simulation::getNextClockCycle(TimeConverter* tc) {
    if ( clockMap.find( tc->getFactor() ) == clockMap.end() ) {
	Output out("Simulation: @R:@t:", 0, 0, Output::STDERR);
	out.fatal(CALL_INFO, -1,
		  "Call to getNextClockCycle() on a clock that was not previously registered, exiting...\n");
    }
    return clockMap[tc->getFactor()]->getNextCycle();
}

void Simulation::unregisterClock(TimeConverter *tc, Clock::HandlerBase* handler) {
//     _SIM_DBG("freq=%f handler=%p\n", frequency, handler );
    
    if ( clockMap.find( tc->getFactor() ) != clockMap.end() ) {
	_SIM_DBG( "\n" );
	bool empty;
	clockMap[ tc->getFactor() ]->unregisterHandler( handler, empty );
	
    }
}

TimeConverter* Simulation::registerOneShot(std::string timeDelay, OneShot::HandlerBase* handler)
{
    return registerOneShot(UnitAlgebra(timeDelay), handler);
}

TimeConverter* Simulation::registerOneShot(const UnitAlgebra& timeDelay, OneShot::HandlerBase* handler)
{
    TimeConverter* tcTimeDelay = timeLord->getTimeConverter(timeDelay);

    // Search the oneShot map for a oneShot with the associated timeDelay factor
    if (oneShotMap.find(tcTimeDelay->getFactor()) == oneShotMap.end()) {
        // OneShot with the specific timeDelay not found, 
        // create a new one and add it to the map of OneShots
        OneShot* ose = new OneShot(tcTimeDelay);
        oneShotMap[tcTimeDelay->getFactor()] = ose; 
    }
    
    // Add the handler to the OneShots list of handlers, Also the
    // registerHandler will schedule the oneShot to fire in the future
    oneShotMap[tcTimeDelay->getFactor()]->registerHandler(handler);
    return tcTimeDelay;
}

void Simulation::insertActivity(SimTime_t time, Activity* ev) {
    ev->setDeliveryTime(time);
    timeVortex->insert(ev);

}

template<class Archive>
void
Simulation::serialize(Archive & ar, const unsigned int version)
{
    printf("begin Simulation::serialize\n");
    ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(SimulationBase);

    ar & BOOST_SERIALIZATION_NVP(output_directory);

    printf("  - Simulation::timeVortex\n");
    ar & BOOST_SERIALIZATION_NVP(timeVortex);

    printf("  - Simulation::sync (%p)\n", sync);
    ar & BOOST_SERIALIZATION_NVP(sync);

    printf("  - Simulation::compMap\n");
    ar & BOOST_SERIALIZATION_NVP(compMap);

    printf("  - Simulation::introMap\n");
    ar & BOOST_SERIALIZATION_NVP(introMap);
    printf("  - Simulation::clockMap\n");
    ar & BOOST_SERIALIZATION_NVP(clockMap);
    printf("  - Simulation::oneShotMap\n");
    ar & BOOST_SERIALIZATION_NVP(oneShotMap);
    printf("  - Simulation::currentSimCycle\n");
    ar & BOOST_SERIALIZATION_NVP(currentSimCycle);
    printf("  - Simulation::m_exit\n");
    ar & BOOST_SERIALIZATION_NVP(m_exit);
    printf("  - Simulation::endSim\n");
    ar & BOOST_SERIALIZATION_NVP(endSim);
    printf("  - Simulation::my_rank\n");
    ar & BOOST_SERIALIZATION_NVP(my_rank);
    printf("  - Simulation::num_ranks\n");
    ar & BOOST_SERIALIZATION_NVP(num_ranks);

    // printf("  - Simulation::compInfoMap\n");
    // ar & BOOST_SERIALIZATION_NVP(compInfoMap);

    printf("end Simulation::serialize\n");
}

} // namespace SST


SST_BOOST_SERIALIZATION_INSTANTIATE(SST::SimulationBase::serialize)
SST_BOOST_SERIALIZATION_INSTANTIATE(SST::Simulation::serialize)

BOOST_CLASS_EXPORT_IMPLEMENT(SST::SimulationBase)
BOOST_CLASS_EXPORT_IMPLEMENT(SST::Simulation)
