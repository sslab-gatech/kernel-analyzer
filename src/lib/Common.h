#ifndef _COMMON_H
#define _COMMON_H

#include <llvm/Support/raw_ostream.h>
#include "Flags.h"

#include <unistd.h>
#include <bitset>
#include <chrono>

#define KA_LOG(lv, stmt)							\
	do {											\
		if (VerboseLevel >= lv)						\
			llvm::errs() << stmt;					\
	} while(0)


#define RES_REPORT(stmt) KA_LOG(0, stmt);
#define WARNING(stmt) KA_LOG(1, "\n[WARN] " << stmt);
#define TEST_REPORT(stmt) KA_LOG(3, "[TEST] " << stmt);

#define KA_ERR(stmt)															\
	do {																		\
		llvm::errs() << "ERROR (" << __FUNCTION__ << "@" << __LINE__ << ")";	\
		llvm::errs() << ": " << stmt;											\
		exit(-1);																\
    } while(0)

class Timer {
public:
	Timer(StringRef name) : name(name), begin(now()) {
	}

	~Timer() {
		auto end = now();
		auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-begin).count();
		errs() << "Timer on " << name << " : " << duration << " milliseconds" << "\n";
	}
private:
	std::chrono::high_resolution_clock::time_point now() {
		return std::chrono::high_resolution_clock::now();
	}
	StringRef name;
	std::chrono::high_resolution_clock::time_point begin;
};

// #define FUNCTION_TIMER() Timer _t_func = Timer(__FUNCTION__)
// #define NAMED_TIMER(name) Timer _t_named = Timer(name)

#define FUNCTION_TIMER()
#define NAMED_TIMER(name)

#endif
