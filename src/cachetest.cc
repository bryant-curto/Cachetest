#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <iomanip>
#include <sched.h>
#include <sys/time.h>
#include <vector>
#include <utility>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <Options.hpp>
#include <Distribution.hpp>
#include <Buffer.hpp>
#include "zipf.h"
#include <Prototypes.hpp>
#include <config.h>

#include <Perf.hpp>
//Defs

#define INITIAL_LOOP_ITERATIONS 10000000
//#define DEBUG
//#define DEBUG_CREATE
//#define DEBUG_RUN
//#define DEBUG_ALARM

#ifdef WRITEBACK
void * WRITEBACK_VAR;
#endif

#define START_CODE 4711u
#define EXIT_CODE 4710u

#define OPT_ARG_STRING "shab:d:f:l:r:c:e:o:Awu"

//Global Variables
unsigned int*       barrier             = NULL;
static cpu_set_t*   cpuset              = NULL;
std::ostream*       output              = NULL;
std::ostream*       error_out           = NULL;
Distribution*       distr               = NULL;
Perf*               perf                = NULL;
std::vector<Event>* Measured_events     = NULL;

Buffer*				buffer;
Result_vector_t     Results;
std::stringstream   ss, ss1;
Options             opt;

//We should add a sanity check to ensure that the dataset is a multiple of 
//the element size, otherwise we might have half sized elements, which could force
//a buggy situtation

void error(std::string code)
{
    std::cerr << code << std::endl;
    exit(1);
}

/* This function alters the indices stored within the buffer so that the
 * pointer chasing path is split into subpaths. Each subpath is a new
 * pointer chasing path, each pair has a difference in lenth <= 1, and
 * the locations spanned by the subpath matches the locations spanned
 * by the original.
 * Returns the starting index of each subpath
 */
std::vector<element_size_t> splitPointerChasingPath(const element_size_t numPaths) {
	auto printPath = [](element_size_t idx) {
		std::cout << "Path:" << std::endl;
		const element_size_t firstIdx = idx;
		size_t length = 0;
		do {
			std::cout << idx << ", ";
			length += 1;
			idx = *(element_size_t *)(buffer->Get_buffer_pointer() + idx);
		} while (firstIdx != idx);

		std::cout << std::endl << "  length=" << length << " vs " << distr->getEntries() << std::endl;
	};

	/*
	// Log input pointer chaising path
	std::cout << "Input ";
	printPath(buffer->Get_start_address() - buffer->Get_buffer_pointer());
	*/

	// Try to evenly split entries in original pointer chaising path
	// amongst the new paths. Assign extras to earlier paths.
	const element_size_t minPathLength = distr->getEntries() / numPaths;
	if (0 == minPathLength) {
		std::cerr << "Can't split path of length " << distr->getEntries() << " into "
				  << numPaths << " parts" << std::endl;
		exit(-1);
	}
	element_size_t extras = distr->getEntries() - minPathLength * numPaths;

	// Figure out the addresses of the first and last entries of each path
	std::vector<std::pair<element_size_t, element_size_t> > pathStartEndIndices;
	element_size_t unusedEntryIdx = buffer->Get_start_address() - buffer->Get_buffer_pointer();
	for (element_size_t i = 0; i < numPaths; i++) {
		const element_size_t startIdx = unusedEntryIdx;
		for (element_size_t j = 1 /* account for starting entry */; j < minPathLength; ++j) {
			unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
		}
		if (extras > 0) {
			unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
			--extras;
		}
		const element_size_t endIdx = unusedEntryIdx;
		unusedEntryIdx = *(element_size_t *)(buffer->Get_buffer_pointer() + unusedEntryIdx);
		pathStartEndIndices.emplace_back(startIdx, endIdx);
	}

	// Make the last entry in each path refer to the first
	for (auto& pathPair : pathStartEndIndices) {
		*(element_size_t*)(buffer->Get_buffer_pointer() + pathPair.second) = pathPair.first;
	}

	// Collect the staring indices to return
	std::vector<element_size_t> pathStartIndices;
	for (auto& pathPair : pathStartEndIndices) {
		pathStartIndices.emplace_back(pathPair.first);
	}

	/*
	// Log output pointer chaising paths
	std::cout << "New paths:" << std::endl;
	for (element_size_t idx : pathStartIndices) {
		printPath(idx);
	}
	*/

	return pathStartIndices;
}

struct LoopResult {
	long long accesscount;
	unsigned int index;
};

LoopResult loop(const unsigned int startIndex) {
    register long long accesscount = 0;
    register unsigned int stop = START_CODE;
    unsigned int dummy=0;

    register unsigned int index = startIndex;
    unsigned char* startAddr = buffer->Get_buffer_pointer();  //This is new, need to get the correct version from the Buffer

    barrier = &dummy;    //Calculate the upper bound for the sig handler to search

    if(Measured_events != NULL && !perf->start())
        error("ERROR: Can't start counters");

    asm ("#//Loop Starts here");
    for (;;) {
        element_size_t next = *(element_size_t*)(startAddr + index);
        //for ( int j = 0; j < loopfactor; j += 1 ) dummy *= next;
#ifdef WRITEBACK
        *(element_size_t*)(startAddr + index + sizeof(int)) = dummy;
#endif
#ifdef DEBUG_RUN
        std::cout << index << ' ' << next << std::endl;
        std::cout << std::hex << (element_size_t*)(startAddr+index) << std::dec << std::endl;
#endif
        index = next;
        accesscount += 1;

        register unsigned int tester;

#if defined(__powerpc__)
        asm volatile ("mr %0,%1" : "=r" (tester) : "r" (stop) );
#else
        asm volatile ("movl %1, %0" : "=r" (tester) : "r" (stop) );    //This tricks the Compiler
#endif
        asm("#Exit");
        if ( tester == EXIT_CODE) break;
    }

	return LoopResult{.accesscount=accesscount, .index=index};
}

int 
main( int argc, char* const argv[] ) {

	Initialize();

    Parse_options(argc, argv, opt);

    // Perform some setup
    if(!Configure_experiment())
        error("ERROR: Configuring");

    if(!Setup_buffer())
        error("ERROR: Setup_buffer");

    if(!Setup_perf())
        error("ERROR: Setup Perf");

    if(!Setup_distribution())
        error("ERROR: Distribution");

    if(!Synchronize_instances())
        error("ERROR: Synchronizing");

    if(!Setup_timeout())
        error("ERROR: Alarm");

	unsigned int startIndex = (unsigned long long)(buffer->Get_start_address() - buffer->Get_buffer_pointer());
    LoopResult loopRes = loop(startIndex);

    //Get the Perf data
	std::vector<Result_t> results;
    if( Measured_events != NULL != 0 && !perf->stop() )
		error("ERROR: Stopping Counters failed");
	if( !perf->read_results(results) )
        error("ERROR: Reading results");

    //Result output
    *output
        << "dataset="  << (opt.dataset >> 10) << std::endl
        << "duration="  << opt.duration << std::endl
        << "access count="  << loopRes.accesscount << std::endl
        << "cacheline="  << opt.cacheline << std::endl
        << "buffer factor="  << opt.bufferfactor << std::endl
        << "loop factor="  << opt.loopfactor << std::endl
        //<< ' ' << dummy
        << "entries/elems="  << distr->getEntries() << '/' << distr->getNumElements() << std::endl
        << "buffer util="  << std::setprecision(3) << ((double)distr->getBufferUtilization()) << std::endl
        << "idx=" << std::setprecision(0) << std::setw(0) << loopRes.index << std::endl;

    *output << "Perf" << std::endl
        << "  dataset="  << (opt.dataset >> 10) << std::endl
        << "  results=";
	for(std::vector<Result_t>::iterator it = results.begin(); it != results.end(); it++)
		*output << " " << *it;
    *output << std::endl;

    buffer->Dump_frames(ss1.str());

    return 0;
}

//Functions/Methods

static void start_handler( int )
{
}

static void alarm_handler( int ) {
    unsigned int sp = 0;
    for (volatile unsigned int *x = &sp; x <= barrier; x += 1) {
#ifdef DEBUG_ALARM
        printf("stack: %d\n", *x);
#endif
        if (*x == START_CODE) *x = EXIT_CODE;
    }
}

static void usage( const char* program ) {
    std::cerr << "usage: " << program << " [options] <dataset in KB> [output file] [error output file]" << std::endl;
    std::cerr << "options:" << std::endl;
    std::cerr << "-a require physically contigous memory (default: no)" << std::endl;
    std::cerr << "-b buffer factor (default: 1)" << std::endl;
    std::cerr << "-d duration in seconds (default: 1)" << std::endl;
    std::cerr << "-f loop factor (default: 1)" << std::endl;
    std::cerr << "-l cache line size in bytes (default: 64)" << std::endl;
    std::cerr << "-r random seed (default: sequential access)" << std::endl;
    std::cerr << "-c CPU ID to pin the experiment to (default: 1, off main)" << std::endl;
    std::cerr << "-e Event list (rx0000yyxx or xx,uyy format)" << std::endl;
    std::cerr << "-h Use Huge pages (default: no) " << std::endl;
    std::cerr << "-o Offset into the start of the buffer" << std::endl;
    std::cerr << "-A Append output when using files" << std::endl;
    std::cerr << "-w Wait for signal before executing" << std::endl;
    std::cerr << "-s Dump the sequence pattern" << std::endl;
    exit(1);
}

void pinExperiment(int cpu) {
    size_t size;

    //FIXME this shouldn't be hardcoded here?
    cpuset = CPU_ALLOC(100);
    if(cpuset == NULL) {
        perror("CPU_ALLOC");
        exit(EXIT_FAILURE);
    }

    size = CPU_ALLOC_SIZE(100);

    CPU_ZERO_S(size, cpuset);
    CPU_SET(cpu, cpuset);

    sched_setaffinity(getpid(),size,cpuset);
}

bool
Parse_options( int argc, char * const *argv, Options &opt)
{
    for (;;) {
        int option = getopt( argc, argv, OPT_ARG_STRING );
        if ( option < 0 ) break;
        switch(option) {
            case 'b': opt.bufferfactor = atoi( optarg ); break;
            case 'd': opt.duration = atoi( optarg ); break;
            case 'f': opt.loopfactor = atoi( optarg ); break;
            case 'l': opt.cacheline = atoi( optarg ); break;
            case 'r': opt.seed = atoi( optarg ); break;
            case 'c': opt.cpu = atoi( optarg ); break;
            case 'e': Measured_events = perf->parseEvents( optarg ) ; break;
            case 'a': opt.cont = 1; break;
            case 'h': opt.huge = 1; break;
            case 'o': opt.bufferoffset = atoi( optarg ); break;
            case 'A': opt.append = true; break;
            case 'w': opt.wait = true; break;
            case 's': opt.dumpSeq = true; break;
            default: usage( argv[0] ); break;
        }
    }

    if ( argc < optind + 1 ) usage( argv[0] );
    opt.dataset = atoi(argv[optind]) << 10;

    std::stringstream oss;
    if ( argc >= optind + 2 ){
        oss << argv[optind+1] << "_" << opt.cpu << ".dat";
        if(opt.append)
            output = new std::ofstream(oss.str().c_str(),std::ios_base::out | std::ios_base::app);
        else
            output = new std::ofstream(oss.str().c_str());
    }//if
    else
        output = &std::cout;

    if ( argc >= optind + 3 ){
        oss << argv[optind+2] << "_" << opt.cpu << ".dat";
        if(opt.append)
            error_out = new std::ofstream(oss.str().c_str(),std::ios_base::out | std::ios_base::app);
        else
            error_out = new std::ofstream(oss.str().c_str());
    }//if
    else
        error_out = &std::cout;
    return true;
}

bool
Configure_experiment()
{
    if ( opt.duration < 1 ) {
        std::cerr << "ERROR: duration must be at least 1" << std::endl;
        exit(1);
    }
    if ( opt.loopfactor < 0 ) {
        std::cerr << "ERROR: loop factor must be at least 0" << std::endl;
        exit(1);
    }
    if ( opt.cacheline < 2 * sizeof(int) ) {
        std::cerr << "ERROR: cacheline must be at least " << 2 * sizeof(int) << std::endl;
        exit(1);
    }

    //Pin this process
    if(opt.cpu >= 0)
        pinExperiment(opt.cpu);
    return true;
}

bool
Setup_buffer()
{   
    //Allocate the buffer array
    //Need to be 100% sure that this is contiguous
    int buffersize = opt.dataset * opt.bufferfactor;
    if(opt.huge)
    	buffer = new Large_buffer( buffersize, opt.cont );
    else
    	buffer = new Small_buffer( buffersize, opt.cont );

    buffer->Set_buffer_offset(opt.bufferoffset);

    //unsigned char* buffer = new unsigned char[buffersize];
    //memset( buffer, 0, buffersize );
#ifdef DEBUG
    std::cout << "Buffer at: " << std::hex << (element_size_t*)buffer->getPtr() << " - " << (element_size_t*)(buffer->getPtr() + buffersize) << std::endl;
#endif

    return true;
}

bool
Setup_perf()
{
    if(Measured_events == NULL)
        return true;

    for(std::vector<Event>::iterator it = Measured_events->begin(); it != Measured_events->end(); it++)
    {
        if(!perf->addEvent( *it ))
            return false;
    }

    return true;
}

bool
Setup_distribution()
{

    // the code below sets up a list of cacheline-aligned and -sized blocks,
    // such that the first (sizeof int) bytes are used as a pointer to the
    // next index, while the next (sizeof int) bytes can be used for a dummy
    // write operation in the runtime loop below
    distr = NULL;
    if ( opt.seed >= 0 ) {
		std::cout << "Uniform distribution used" << std::endl;
        distr = Distribution::createDistribution(Distribution::UNIFORM,buffer,opt.cacheline,sizeof(element_size_t),opt.seed);
    }else{
		std::cout << "Uniform distribution used" << std::endl;
        distr = Distribution::createDistribution(Distribution::LINEAR,buffer,opt.cacheline,sizeof(element_size_t),opt.seed);
    }
    distr->distribute();

#ifdef DEBUG_CREATE
    int dumpfd = open("bufferdump.dat",O_CREAT | O_RDWR, S_IRWXU);
    distr->dumpBuffer(dumpfd);
#endif
    ss1 << "frameDump_" << opt.cpu << ".dat";
    ss << "sequencedump_" << opt.cpu << ".dat";
    unlink(ss.str().c_str());

    int seqdumpfd = open(ss.str().c_str(),O_CREAT | O_RDWR, S_IRWXU);
    if(opt.dumpSeq)
        distr->dumpSequence(seqdumpfd); 
    return true;
}

bool
Synchronize_instances()
{
    if(opt.wait) {
        //At this point I want to wait for the signal
        signal(SIGUSR1, start_handler);
        pause();
    }
    return true;
}

bool
Setup_timeout()
{
    //Setup the alarm signal handler
    if ( (signal( SIGALRM, alarm_handler ) < 0) || (alarm( opt.duration ) < 0) ) {
        std::cerr << "ERROR: signal or alarm" << std::endl;
        return false;
    }
    return true;
}

bool
Perform_experiment()
{
    return true;
}

bool
Dump_results()
{
    return true;
}

void
Initialize()
{
	perf = Perf::get_instance();
}
