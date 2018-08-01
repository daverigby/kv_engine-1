/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_SUITE_STUBS_H_
#define SRC_SUITE_STUBS_H_ 1

#include "config.h"

#include <platform/cbassert.h>

#include "ep_testsuite.h"

bool teardown(EngineIface* h, EngineIface* h1);
void delay(int amt);

void add(EngineIface* h, EngineIface* h1);
void append(EngineIface* h, EngineIface* h1);
void decr(EngineIface* h, EngineIface* h1);
void decrWithDefault(EngineIface* h, EngineIface* h1);
void prepend(EngineIface* h, EngineIface* h1);
void flush(EngineIface* h, EngineIface* h1);
void del(EngineIface* h, EngineIface* h1);
void get(EngineIface* h, EngineIface* h1);
void set(EngineIface* h, EngineIface* h1);
void setRetainCAS(EngineIface* h, EngineIface* h1);
void incr(EngineIface* h, EngineIface* h1);
void incrWithDefault(EngineIface* h, EngineIface* h1);
void getLock(EngineIface* h, EngineIface* h1);
void setUsingCAS(EngineIface* h, EngineIface* h1);
void deleteUsingCAS(EngineIface* h, EngineIface* h1);
void appendUsingCAS(EngineIface* h, EngineIface* h1);
void prependUsingCAS(EngineIface* h, EngineIface* h1);

void checkValue(EngineIface* h, EngineIface* h1, const char* exp);
void assertNotExists(EngineIface* h, EngineIface* h1);

#define assertHasError() cb_assert(hasError)
#define assertHasNoError() cb_assert(!hasError)

extern int expiry;
extern int locktime;
extern bool hasError;
extern uint64_t cas;
extern struct test_harness testHarness;

engine_test_t* get_tests_0(void);
engine_test_t* get_tests_1(void);
engine_test_t* get_tests_2(void);
engine_test_t* get_tests_3(void);
engine_test_t* get_tests_4(void);
engine_test_t* get_tests_5(void);
engine_test_t* get_tests_6(void);
engine_test_t* get_tests_7(void);
engine_test_t* get_tests_8(void);
engine_test_t* get_tests_9(void);

#endif  /* SRC_SUITE_STUBS_H_ */
