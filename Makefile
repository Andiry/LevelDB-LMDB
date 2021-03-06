# Copyright (c) 2011 The LevelDB Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file. See the AUTHORS file for names of contributors.

#-----------------------------------------------
# Uncomment exactly one of the lines labelled (A), (B), and (C) below
# to switch between compilation modes.

OPT ?= -O2 -DNDEBUG -std=c++11       # (A) Production use (optimized mode)
# OPT ?= -g2              # (B) Debug mode, w/ full line-level debugging symbols
# OPT ?= -O2 -g2 -DNDEBUG # (C) Profiling mode: opt, but w/debugging symbols
#-----------------------------------------------

# detect what platform we're building on
$(shell CC="$(CC)" CXX="$(CXX)" TARGET_OS="$(TARGET_OS)" \
    ./build_detect_platform build_config.mk ./)
# this file is generated by the previous line to set build flags and sources
include build_config.mk

CFLAGS += -I. -I./include $(PLATFORM_CCFLAGS) $(OPT)
CXXFLAGS += -I. -I./include $(PLATFORM_CXXFLAGS) $(OPT)

LDFLAGS += $(PLATFORM_LDFLAGS)
LIBS += $(PLATFORM_LIBS)

LIBOBJECTS = $(SOURCES:.cc=.o)
MEMENVOBJECTS = $(MEMENV_SOURCES:.cc=.o)

TESTUTIL = ./util/testutil.o
TESTHARNESS = ./util/testharness.o $(TESTUTIL)

TESTS = \
	arena_test \
	autocompact_test \
	bloom_test \
	c_test \
	cache_test \
	coding_test \
	corruption_test \
	crc32c_test \
	db_test \
	dbformat_test \
	env_test \
	filename_test \
	filter_block_test \
	issue178_test \
	issue200_test \
	log_test \
	memenv_test \
	skiplist_test \
	table_test \
	version_edit_test \
	version_set_test \
	write_batch_test

PROGRAMS = db_bench leveldbutil $(TESTS)
BENCHMARKS = db_bench db_bench_sqlite3 db_bench_tree_db db_bench_mdb \
	db_bench_bdb db_bench_sophia db_bench_ntdb db_bench_tdb db_bench_tokudb \
	db_bench_basho db_bench_hyper db_bench_rocksdb db_bench_wiredtiger \
	db_bench_mdbm

BASHO = ../basho_leveldb
HYPER = ../HyperLevelDB
ROCKSDB = ../rocksdb

LIBRARY = libleveldb.a
MEMENVLIBRARY = libmemenv.a

default: all

# Should we build shared libraries?
ifneq ($(PLATFORM_SHARED_EXT),)

ifneq ($(PLATFORM_SHARED_VERSIONED),true)
SHARED1 = libleveldb.$(PLATFORM_SHARED_EXT)
SHARED2 = $(SHARED1)
SHARED3 = $(SHARED1)
SHARED = $(SHARED1)
else
# Update db.h if you change these.
SHARED_MAJOR = 1
SHARED_MINOR = 17
SHARED1 = libleveldb.$(PLATFORM_SHARED_EXT)
SHARED2 = $(SHARED1).$(SHARED_MAJOR)
SHARED3 = $(SHARED1).$(SHARED_MAJOR).$(SHARED_MINOR)
SHARED = $(SHARED1) $(SHARED2) $(SHARED3)
$(SHARED1): $(SHARED3)
	ln -fs $(SHARED3) $(SHARED1)
$(SHARED2): $(SHARED3)
	ln -fs $(SHARED3) $(SHARED2)
endif

$(SHARED3):
	$(CXX) $(LDFLAGS) $(PLATFORM_SHARED_LDFLAGS)$(SHARED2) $(CXXFLAGS) $(PLATFORM_SHARED_CFLAGS) $(SOURCES) -o $(SHARED3) $(LIBS)

endif  # PLATFORM_SHARED_EXT

all: $(SHARED) $(LIBRARY)

check: all $(PROGRAMS) $(TESTS)
	for t in $(TESTS); do echo "***** Running $$t"; ./$$t || exit 1; done

clean:
	-rm -f $(PROGRAMS) $(BENCHMARKS) $(LIBRARY) $(SHARED) $(MEMENVLIBRARY) */*.o */*/*.o ios-x86/*/*.o ios-arm/*/*.o build_config.mk
	-rm -rf ios-x86/* ios-arm/*

$(LIBRARY): $(LIBOBJECTS)
	rm -f $@
	$(AR) -rs $@ $(LIBOBJECTS)

bench: $(BENCHMARKS)

clean-bench:
	rm -f doc/bench/*.o $(BENCHMARKS)

db_bench: db/db_bench.o $(LIBOBJECTS) $(TESTUTIL)
	$(CXX) $(LDFLAGS) db/db_bench.o $(LIBOBJECTS) $(TESTUTIL) -o $@ $(LIBS)

db_bench_basho: doc/bench/db_bench_basho.o $(BASHO)/libleveldb.a $(BASHO)/$(TESTUTIL)
	$(CXX) $(LDFLAGS) doc/bench/db_bench_basho.o $(BASHO)/libleveldb.a $(BASHO)/$(TESTUTIL) -o $@ $(LIBS) -lrt

doc/bench/db_bench_basho.o: doc/bench/db_bench_basho.cc
	$(CXX) -I$(BASHO) -I$(BASHO)/include $(CXXFLAGS) -c $< -o $@
	
db_bench_hyper: doc/bench/db_bench_hyper.o $(HYPER)/.libs/libhyperleveldb.a $(HYPER)/$(TESTUTIL)
	$(CXX) $(LDFLAGS) doc/bench/db_bench_hyper.o $(HYPER)/.libs/libhyperleveldb.a $(HYPER)/$(TESTUTIL) -o $@ $(LIBS) -lrt

doc/bench/db_bench_hyper.o: doc/bench/db_bench_hyper.cc
	$(CXX) -I$(HYPER) -I$(HYPER)/include $(CXXFLAGS) -DHAVE_CONFIG_H -c $< -o $@

db_bench_rocksdb: doc/bench/db_bench_rocksdb.o $(ROCKSDB)/librocksdb.a $(ROCKSDB)/$(TESTUTIL)
	$(CXX) $(LDFLAGS) doc/bench/db_bench_rocksdb.o $(ROCKSDB)/librocksdb.a $(ROCKSDB)/$(TESTUTIL) -o $@ $(LIBS) -lrt

$(ROCKSDB)/librocksdb.a:
	make -C $(ROCKSDB) static_lib

$(ROCKSDB)/$(TESTUTIL):
	make -C $(ROCKSDB) $(TESTUTIL)

doc/bench/db_bench_rocksdb.o: doc/bench/db_bench_rocksdb.cc
	$(CXX) -std=c++11 -I$(ROCKSDB) -I$(ROCKSDB)/include -DROCKSDB_PLATFORM_POSIX -DROCKSDB_ATOMIC_PRESENT -DROCKSDB_FALLOCATE_PRESENT $(CXXFLAGS) -c $< -o $@

db_bench_sqlite3: doc/bench/db_bench_sqlite3.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_sqlite3.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libsqlite3.a -ldl

db_bench_sqlo: doc/bench/db_bench_sqlo.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_sqlo.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libsqlite3.a -ldl

db_bench_sqlm: doc/bench/db_bench_sqlo.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_sqlo.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libsqlite3lmdb.a -ldl

db_bench_tree_db: doc/bench/db_bench_tree_db.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_tree_db.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/lib/x86_64-linux-gnu/libkyotocabinet.a /usr/lib/x86_64-linux-gnu/libz.so /usr/lib/x86_64-linux-gnu/liblzma.so /usr/lib/x86_64-linux-gnu/liblzo2.so

db_bench_forestdb: doc/bench/db_bench_forestdb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_forestdb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libforestdb.a

db_bench_mdb: doc/bench/db_bench_mdb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_mdb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/liblmdb.a /usr/lib/x86_64-linux-gnu/libsnappy.so

db_bench_bdb: doc/bench/db_bench_bdb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_bdb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/lib/x86_64-linux-gnu/libdb.a

db_bench_sophia: doc/bench/db_bench_sophia.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_sophia.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libsophia.a

db_bench_ntdb: doc/bench/db_bench_ntdb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_ntdb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libntdb.a

db_bench_tdb: doc/bench/db_bench_tdb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_tdb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libtdb.a

db_bench_tokudb: doc/bench/db_bench_tokudb.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_tokudb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libtokufractaltree_static.a /usr/local/lib/libtokuportability_static.a -ldl -lz

db_bench_ydb: doc/bench/db_bench_ydb.o $(LIBRARY) $(TESTUTIL)
#	$(CXX) doc/bench/db_bench_ydb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libtokufractaltree_static.a /usr/local/lib/libtokuportability_static.a /usr/local/lib/libjemalloc.a -ldl -lz
	$(CXX) doc/bench/db_bench_ydb.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) -ltokufractaltree -ltokuportability -ljemalloc -ldl -lz

db_bench_wiredtiger: doc/bench/db_bench_wiredtiger.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_wiredtiger.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libwiredtiger.a -ldl

db_bench_mdbm: doc/bench/db_bench_mdbm.o $(LIBRARY) $(TESTUTIL)
	$(CXX) doc/bench/db_bench_mdbm.o $(LIBRARY) $(TESTUTIL) -o $@ $(LDFLAGS) /usr/local/lib/libmdbm.a -lcrypto

doc/bench/db_bench_mdbm.o: doc/bench/db_bench_mdbm.cc
	$(CXX) -I/usr/local/include/mdbm $(CXXFLAGS) -DHAVE_CONFIG_H -c $< -o $@

leveldbutil: db/leveldb_main.o $(LIBOBJECTS)
	$(CXX) $(LDFLAGS) db/leveldb_main.o $(LIBOBJECTS) -o $@ $(LIBS)

arena_test: util/arena_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/arena_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

autocompact_test: db/autocompact_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/autocompact_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

bloom_test: util/bloom_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/bloom_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

c_test: db/c_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/c_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

cache_test: util/cache_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/cache_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

coding_test: util/coding_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/coding_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

corruption_test: db/corruption_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/corruption_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

crc32c_test: util/crc32c_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/crc32c_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

db_test: db/db_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/db_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

dbformat_test: db/dbformat_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/dbformat_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

env_test: util/env_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) util/env_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

filename_test: db/filename_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/filename_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

filter_block_test: table/filter_block_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) table/filter_block_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

issue178_test: issues/issue178_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) issues/issue178_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

issue200_test: issues/issue200_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) issues/issue200_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

log_test: db/log_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/log_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

table_test: table/table_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) table/table_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

skiplist_test: db/skiplist_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/skiplist_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

version_edit_test: db/version_edit_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/version_edit_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

version_set_test: db/version_set_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/version_set_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

write_batch_test: db/write_batch_test.o $(LIBOBJECTS) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) db/write_batch_test.o $(LIBOBJECTS) $(TESTHARNESS) -o $@ $(LIBS)

$(MEMENVLIBRARY) : $(MEMENVOBJECTS)
	rm -f $@
	$(AR) -rs $@ $(MEMENVOBJECTS)

memenv_test : helpers/memenv/memenv_test.o $(MEMENVLIBRARY) $(LIBRARY) $(TESTHARNESS)
	$(CXX) $(LDFLAGS) helpers/memenv/memenv_test.o $(MEMENVLIBRARY) $(LIBRARY) $(TESTHARNESS) -o $@ $(LIBS)

ifeq ($(PLATFORM), IOS)
# For iOS, create universal object files to be used on both the simulator and
# a device.
PLATFORMSROOT=/Applications/Xcode.app/Contents/Developer/Platforms
SIMULATORROOT=$(PLATFORMSROOT)/iPhoneSimulator.platform/Developer
DEVICEROOT=$(PLATFORMSROOT)/iPhoneOS.platform/Developer
IOSVERSION=$(shell defaults read $(PLATFORMSROOT)/iPhoneOS.platform/version CFBundleShortVersionString)
IOSARCH=-arch armv6 -arch armv7 -arch armv7s -arch arm64

.cc.o:
	mkdir -p ios-x86/$(dir $@)
	$(CXX) $(CXXFLAGS) -isysroot $(SIMULATORROOT)/SDKs/iPhoneSimulator$(IOSVERSION).sdk -arch i686 -arch x86_64 -c $< -o ios-x86/$@
	mkdir -p ios-arm/$(dir $@)
	xcrun -sdk iphoneos $(CXX) $(CXXFLAGS) -isysroot $(DEVICEROOT)/SDKs/iPhoneOS$(IOSVERSION).sdk $(IOSARCH) -c $< -o ios-arm/$@
	lipo ios-x86/$@ ios-arm/$@ -create -output $@

.c.o:
	mkdir -p ios-x86/$(dir $@)
	$(CC) $(CFLAGS) -isysroot $(SIMULATORROOT)/SDKs/iPhoneSimulator$(IOSVERSION).sdk -arch i686 -arch x86_64 -c $< -o ios-x86/$@
	mkdir -p ios-arm/$(dir $@)
	xcrun -sdk iphoneos $(CC) $(CFLAGS) -isysroot $(DEVICEROOT)/SDKs/iPhoneOS$(IOSVERSION).sdk $(IOSARCH) -c $< -o ios-arm/$@
	lipo ios-x86/$@ ios-arm/$@ -create -output $@

else
.cc.o:
	$(CXX) $(CXXFLAGS) -c $< -o $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@
endif
