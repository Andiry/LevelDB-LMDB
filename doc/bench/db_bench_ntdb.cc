// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Copyright (c) 2014 Howard Chu @ Symas Corp. Terms as above.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ntdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "port/port.h"
#include "util/histogram.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/testutil.h"

// Comma-separated list of operations to run in the specified order
//   Actual benchmarks:
//
//   fillseq       -- write N values in sequential key order in async mode
//   fillrandom    -- write N values in random key order in async mode
//   fillrandint   -- write N values in random binary key order in async mode
//   overwrite     -- overwrite N values in random key order in async mode
//   fillseqsync   -- write N/100 values in sequential key order in sync mode
//   fillseqbatch  -- batch write N values in sequential key order in async mode
//   fillrandsync  -- write N/100 values in random key order in sync mode
//   fillrandbatch  -- batch write N values in random key order in async mode
//   fillseq100K   -- write N/1000 100K values in seq order in async mode
//   readseq       -- read N times sequentially
//   readreverse   -- read N times in reverse order
//   readseq100K   -- read N/1000 100K values in sequential order in async mode
//   readrand100K  -- read N/1000 100K values in sequential order in async mode
//   readrandom    -- read N times in random order
static const char* FLAGS_benchmarks =
    "fillrandsync,"
    "fillrandom,"
    "fillrandbatch,"
    "fillseqsync,"
    "fillseq,"
    "fillseqbatch,"
    "overwrite,"
#if 0
    "overwritebatch,"
#endif
    "readrandom,"
    "readseq,"
#if 0
    "readreverse,"
    "fillrand100K,"
    "fillseq100K,"
    "readseq100K,"
    "readrand100K,"
#endif
    ;

// Number of key/values to place in database
static int64_t FLAGS_num = 1000000;

// Number of read operations to do.  If negative, do FLAGS_num reads.
static int64_t FLAGS_reads = -1;

static int FLAGS_cleanmem = 0;

// Number of concurrent threads to run.
static int FLAGS_threads = 1;

// Time in seconds for the random-ops tests to run.
static int FLAGS_duration = 0;

// Per-thread rate limit on writes per second.
// Only for the readwhilewriting test.
static int FLAGS_writes_per_second;

// Stats are reported every N operations when this is
// greater than zero. When 0 the interval grows over time.
static int FLAGS_stats_interval = 0;

// Size of each value
static int FLAGS_value_size = 100;

// Number of key/values to place in database
static int FLAGS_batch = 1000;

// Arrange to generate values that shrink to this fraction of
// their original size after compression
static double FLAGS_compression_ratio = 0.5;

// Print histogram of operation timings
static bool FLAGS_histogram = false;

// Page size. Default 1 KB
static int FLAGS_page_size = 1024;

static int FLAGS_hash_size = 100000;

// If true, do not destroy the existing database.  If you set this
// flag and also specify a benchmark that wants a fresh database, that
// benchmark will fail.
static bool FLAGS_use_existing_db = false;

// If true, we allow batch writes to occur
static bool FLAGS_transaction = true;

// If false, skip sync of meta pages on synchronous writes
static bool FLAGS_metasync = true;

// If true, use writable mmap
static bool FLAGS_writemap = true;

// The Linux kernel does readahead by default
static bool FLAGS_readahead = true;

// If true, use binary integer keys
static bool FLAGS_intkey = false;

// Use the db with the following name.
static const char* FLAGS_db = NULL;

// If true, use shuffle instead of original random algorithm.
// Guarantees full set of unique data, but uses memory for the
// shuffle array, not feasible for larger tests.
static bool FLAGS_shuffle = false;

static bool fresh_db;

static int *shuff = NULL;

// Array of seeds for RNG, one per thread
static uint32_t *seeds;

namespace leveldb {

// Helper for quickly generating random data.
namespace {
class RandomGenerator {
 private:
  std::string data_;
  int pos_;

 public:
  RandomGenerator() {
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < 1048576) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      test::CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice Generate(int len) {
    if (pos_ + len > data_.size()) {
      pos_ = 0;
      assert(len < data_.size());
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }
};

static Slice TrimSpace(Slice s) {
  int start = 0;
  while (start < s.size() && isspace(s[start])) {
    start++;
  }
  int limit = s.size();
  while (limit > start && isspace(s[limit-1])) {
    limit--;
  }
  return Slice(s.data() + start, limit - start);
}

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

struct Stat0 {
  double start;
  double finish;
  double seconds;
  size_t done;
  int64_t bytes;
};

class Stats {
 private:
  int id_;
  size_t last_report_done_;
  int64_t next_report_;
  double last_op_finish_;
  double last_report_finish_;
  Histogram hist_;
  std::string message_;
  bool exclude_from_merge_;

 public:
  double start_;
  double finish_;
  double seconds_;
  size_t done_;
  int64_t bytes_;
  Stats() { Start(-1); }

  void Start(int id) {
    id_ = id;
    next_report_ = FLAGS_stats_interval ? FLAGS_stats_interval : 100;
    last_op_finish_ = start_;
    hist_.Clear();
    done_ = 0;
	last_report_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = Env::Default()->NowMicros();
    finish_ = start_;
	last_report_finish_ = start_;
    message_.clear();
	// When set, stats from this thread won't be merged with others.
	exclude_from_merge_ = false;
  }

  void Merge(const Stats& other) {
	if (other.exclude_from_merge_)
		return;

    hist_.Merge(other.hist_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = Env::Default()->NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) {
    AppendWithSpace(&message_, msg);
  }

  void SetExcludeFromMerge() { exclude_from_merge_ = true; }

  void TimeStr(time_t seconds, char *buf) {
	struct tm tm;
	localtime_r(&seconds, &tm);
	sprintf(buf, "%04d/%02d/%02d-%02d:%02d:%02d",
		tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
  }

  void FinishedSingleOp() {
    if (FLAGS_histogram) {
      double now = Env::Default()->NowMicros();
      double micros = now - last_op_finish_;
      hist_.Add(micros);
      if (micros > 20000) {
        fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_++;
    if (done_ >= next_report_) {
	  if (!FLAGS_stats_interval) {
      if      (next_report_ < 1000)   next_report_ += 100;
      else if (next_report_ < 5000)   next_report_ += 500;
      else if (next_report_ < 10000)  next_report_ += 1000;
      else if (next_report_ < 50000)  next_report_ += 5000;
      else if (next_report_ < 100000) next_report_ += 10000;
      else if (next_report_ < 500000) next_report_ += 50000;
      else                            next_report_ += 100000;
      fprintf(stderr, "... finished %zd ops%30s\r", done_, "");
      fflush(stderr);
	  } else {
        double now = Env::Default()->NowMicros();
		char buf[20];
		TimeStr((int) (now/1000000), buf);
        fprintf(stderr,
                "%s ... thread %d: (%zd,%zd) ops and "
                "(%.1f,%.1f) ops/second in (%.6f,%.6f) seconds\n",
                buf,
                id_,
                done_ - last_report_done_, done_,
                (done_ - last_report_done_) /
                ((now - last_report_finish_) / 1000000.0),
                done_ / ((now - start_) / 1000000.0),
                (now - last_report_finish_) / 1000000.0,
                (now - start_) / 1000000.0);
        fflush(stderr);
        next_report_ += FLAGS_stats_interval;
        last_report_finish_ = now;
        last_report_done_ = done_;
	  }
    }
  }

  void AddBytes(int64_t n) {
    bytes_ += n;
  }

  void Report(const Slice& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedSingleOp().
    if (done_ < 1) done_ = 1;

    std::string extra;
    double elapsed = (finish_ - start_) * 1e-6;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    double throughput = (double)done_/elapsed;

    fprintf(stdout, "%-12s : %11.3f micros/op %ld ops/sec;%s%s\n",
            name.ToString().c_str(),
            elapsed * 1e6 / done_,
			(long)throughput,
            (extra.empty() ? "" : " "),
            extra.c_str());
    if (FLAGS_histogram) {
      fprintf(stdout, "Microseconds per op:\n%s\n", hist_.ToString().c_str());
    }
    fflush(stdout);
  }
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  port::Mutex mu;
  port::CondVar cv;
  int total;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  int num_initialized;
  int num_done;
  bool start;

  SharedState() : cv(&mu) { }
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;             // 0..n-1 when running in n threads
  Random64 rand;         // Has different seeds for different threads
  Stats stats;
  SharedState* shared;

  ThreadState(int index)
      : tid(index),
        rand(seeds[index]) {
  }
};

class Duration {
 public:
  Duration(int max_seconds, int64_t max_ops) {
    max_seconds_ = max_seconds;
    max_ops_= max_ops;
    ops_ = 0;
    start_at_ = Env::Default()->NowMicros();
  }

  bool Done(int64_t increment) {
    if (increment <= 0) increment = 1;    // avoid Done(0) and infinite loops
    ops_ += increment;

    if (max_seconds_) {
      // Recheck every appx 1000 ops (exact iff increment is factor of 1000)
      if ((ops_/1000) != ((ops_-increment)/1000)) {
        double now = Env::Default()->NowMicros();
        return ((now - start_at_) / 1000000.0) >= max_seconds_;
      } else {
        return false;
      }
    } else {
      return ops_ > max_ops_;
    }
  }

 private:
  int max_seconds_;
  int64_t max_ops_;
  int64_t ops_;
  double start_at_;
};
}  // namespace

class Benchmark {
 public:
  enum Order {
    SEQUENTIAL,
    RANDOM
  };
  enum DBFlags {
    NONE = 0,
	SYNC,
	RDONLY
  };

 private:
  struct ntdb_context *db_;
  int db_num_;
  int64_t num_;
  int value_size_;
  int entries_per_batch_;
  int64_t reads_;
  DBFlags dbflags_;
  Order write_order_;

  void PrintHeader() {
    const int kKeySize = FLAGS_intkey ? sizeof(int) : 16;
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", kKeySize);
    fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
            FLAGS_value_size,
            static_cast<int>(FLAGS_value_size * FLAGS_compression_ratio + 0.5));
    fprintf(stdout, "Entries:    %ld\n", num_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(kKeySize + FLAGS_value_size) * num_)
             / 1048576.0));
    fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
            (((kKeySize + FLAGS_value_size * FLAGS_compression_ratio) * num_)
             / 1048576.0));
    PrintWarnings();
    fprintf(stdout, "------------------------------------------------\n");
  }

  void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    fprintf(stdout,
            "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n"
            );
#endif
#ifndef NDEBUG
    fprintf(stdout,
            "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
  }

  void PrintEnvironment() {
    fprintf(stderr, "NTDB:       version 1.0\n");

#if defined(__linux)
    time_t now = time(NULL);
    fprintf(stderr, "Date:       %s", ctime(&now));  // ctime() adds newline

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != NULL) {
        const char* sep = strchr(line, ':');
        if (sep == NULL) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

 public:
  Benchmark()
  : db_(NULL),
    db_num_(0),
    num_(FLAGS_num),
	value_size_(FLAGS_value_size),
	entries_per_batch_(1),
    reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads) {
    std::vector<std::string> files;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    Env::Default()->GetChildren(test_dir.c_str(), &files);
    if (!FLAGS_use_existing_db) {
      for (int i = 0; i < files.size(); i++) {
        if (Slice(files[i]).starts_with("dbbench_mdb")) {
          std::string file_name(test_dir);
          file_name += "/";
          file_name += files[i];
          Env::Default()->DeleteFile(file_name.c_str());
        }
      }
    }
  }

  ~Benchmark() {
  	ntdb_close(db_);
  }

  void Run() {
    PrintHeader();

    const char* benchmarks = FLAGS_benchmarks;
    while (benchmarks != NULL) {
      const char* sep = strchr(benchmarks, ',');
      Slice name;
      if (sep == NULL) {
        name = benchmarks;
        benchmarks = NULL;
      } else {
        name = Slice(benchmarks, sep - benchmarks);
        benchmarks = sep + 1;
      }

	  num_ = FLAGS_num;
	  reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
	  value_size_ = FLAGS_value_size;
	  entries_per_batch_ = 1;

	  void (Benchmark::*method)(ThreadState*) = NULL;
	  fresh_db = false;
	  int num_threads = FLAGS_threads;

	  dbflags_ = NONE;
      if (name == Slice("fillseq")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseqbatch")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandom")) {
		fresh_db = true;
		write_order_ = RANDOM;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandbatch")) {
		fresh_db = true;
		write_order_ = RANDOM;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("overwrite")) {
		write_order_ = RANDOM;
		method = &Benchmark::Write;
      } else if (name == Slice("overwritebatch")) {
		write_order_ = RANDOM;
		entries_per_batch_ = FLAGS_batch;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrandsync")) {
		fresh_db = true;
#if 1
		num_ /= 1000;
		if (num_<10) num_=10;
#endif
		write_order_ = RANDOM;
        dbflags_ = SYNC;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseqsync")) {
		fresh_db = true;
#if 1
		num_ /= 1000;
		if (num_<10) num_=10;
#endif
		write_order_ = SEQUENTIAL;
        dbflags_ = SYNC;
		method = &Benchmark::Write;
      } else if (name == Slice("fillrand100K")) {
		fresh_db = true;
		write_order_ = RANDOM;
		num_ /= 1000;
		value_size_ = 100 * 1000;
		method = &Benchmark::Write;
      } else if (name == Slice("fillseq100K")) {
		fresh_db = true;
		write_order_ = SEQUENTIAL;
		num_ /= 1000;
		value_size_ = 100 * 1000;
		method = &Benchmark::Write;
      } else if (name == Slice("readseq")) {
		dbflags_ = RDONLY;
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readreverse")) {
		dbflags_ = RDONLY;
        method = &Benchmark::ReadReverse;
      } else if (name == Slice("readrandom")) {
		dbflags_ = RDONLY;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readrand100K")) {
		dbflags_ = RDONLY;
        reads_ /= 1000;
        method = &Benchmark::ReadRandom;
      } else if (name == Slice("readseq100K")) {
		dbflags_ = RDONLY;
        reads_ /= 1000;
        method = &Benchmark::ReadSequential;
      } else if (name == Slice("readwhilewriting")) {
	    num_threads++;
        method = &Benchmark::ReadWhileWriting;
      } else {
        if (name != Slice()) {  // No error message for empty name
          fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
        }
      }

    if (fresh_db) {
      if (FLAGS_use_existing_db) {
        fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
		        name.ToString().c_str());
		method = NULL;
      }
	  if (db_) {
		  char cmd[200];
		  sprintf(cmd, "rm -rf %s*", FLAGS_db);
		  ntdb_close(db_);
		  if (system(cmd)) exit(1);
		  db_ = NULL;
	  }
    }
	if (!db_)
		Open(dbflags_);

	if (method != NULL) {
		RunBenchmark(num_threads, name, method);
		if (method == &Benchmark::Write) {
		  char cmd[200];
		  std::string test_dir;
		  Env::Default()->GetTestDirectory(&test_dir);
		  sprintf(cmd, "du %s", test_dir.c_str());
		  if (system(cmd)) exit(1);
		}
    }
    }
  }

 private:
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    thread->stats.Start(thread->tid);
    (arg->bm->*(arg->method))(thread);
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  void RunBenchmark(int n, Slice name,
                    void (Benchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;

    ThreadArg* arg = new ThreadArg[n];
    for (int i = 0; i < n; i++) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      arg[i].thread = new ThreadState(i);
      arg[i].thread->shared = &shared;
      Env::Default()->StartThread(ThreadBody, &arg[i]);
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    for (int i = 1; i < n; i++) {
      arg[0].thread->stats.Merge(arg[i].thread->stats);
    }
    arg[0].thread->stats.Report(name);

    for (int i = 0; i < n; i++) {
	  seeds[i] = arg[i].thread->rand.Next();
      delete arg[i].thread;
    }
    delete[] arg;
  }

    void Open(DBFlags flags) {
    assert(db_ == NULL);
	int rc, tflags = 0, oflags = 0;
	union ntdb_attribute hashsize;

    char file_name[100], cmd[200];
    db_num_++;
    std::string test_dir;
    Env::Default()->GetTestDirectory(&test_dir);
    snprintf(file_name, sizeof(file_name),
             "%s/dbbench_ntdb-%d",
             test_dir.c_str(),
             db_num_);

	sprintf(cmd, "mkdir -p %s", file_name);
	if (system(cmd)) exit(1);

	if (flags != SYNC)
		tflags |= NTDB_NOSYNC;
	if (flags == RDONLY)
		oflags = O_RDONLY;
	else
		oflags = O_RDWR|O_CREAT;

	hashsize.base.attr = NTDB_ATTRIBUTE_HASHSIZE;
	hashsize.base.next = NULL;
	hashsize.hashsize.size = FLAGS_hash_size;
	strcat(file_name, "/data.ntdb");
	db_ = ntdb_open(file_name, tflags, oflags, 0664, &hashsize);
	if (!db_) {
      fprintf(stderr, "ntdb_open failed\n");
	  exit(1);
    }
  }

  void Write(ThreadState *thread) {
  	const int test_duration = write_order_ == RANDOM ? FLAGS_duration : 0;

	Duration duration(test_duration, num_);
    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%ld ops)", num_);
      thread->stats.AddMessage(msg);
    }

	if (write_order_ == RANDOM && shuff)
	  thread->rand.Shuffle(shuff, num_);

	RandomGenerator gen;
	TDB_DATA tkey, tdata;
	char key[100];
	tkey.dptr = (unsigned char *)key;
	int64_t bytes = 0;
    // Write to database
	unsigned long i = 0;
	while (!duration.Done(entries_per_batch_)) {
	  ntdb_transaction_start(db_);
	  
	  for (int j=0; j < entries_per_batch_; j++) {

      const unsigned long k = (write_order_ == SEQUENTIAL) ? i+j : (shuff ? shuff[i+j] : (thread->rand.Next() % FLAGS_num));
	  int rc;
		  tkey.dsize = snprintf(key, sizeof(key), "%016lx", k);
      bytes += value_size_ + tkey.dsize;

	  tdata.dptr = (unsigned char *)gen.Generate(value_size_).data();
	  tdata.dsize = value_size_;
	  rc = ntdb_store(db_, tkey, tdata, 0);
      if (rc) {
        fprintf(stderr, "set error: %s\n", ntdb_errorstr((enum NTDB_ERROR)rc));
		break;
      }
      thread->stats.FinishedSingleOp();
	  }
	  ntdb_transaction_commit(db_);
	  i += entries_per_batch_;
    }
	thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState *thread) {
  	/* not supported */
  }

  void ReadSequential(ThreadState *thread) {
  	TDB_DATA key, data;
	int64_t bytes = 0;
	int rc;

	ntdb_transaction_start(db_);
	for (rc = ntdb_firstkey(db_, &key); !rc; rc = ntdb_nextkey(db_, &key)) {
		data.dsize = 0;
		rc = ntdb_fetch(db_, key, &data);
		bytes += key.dsize + data.dsize;
		free(data.dptr);
	  thread->stats.FinishedSingleOp();
	}
	ntdb_transaction_cancel(db_);
	thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState *thread) {
  	TDB_DATA key, data;
	int64_t bytes = 0;
	size_t read = 0;
	size_t found = 0;
    char ckey[100];
	int rc;

	key.dptr = (unsigned char *)ckey;

	Duration duration(FLAGS_duration, reads_);
    while (!duration.Done(1)) {
      const unsigned long k = thread->rand.Next() % FLAGS_num;
	  key.dsize = snprintf(ckey, sizeof(ckey), "%016lx", k);
	  read++;
	  rc = ntdb_fetch(db_, key, &data);
	  if (!rc) {
		found++;
		free(data.dptr);
	  }
      thread->stats.FinishedSingleOp();
    }
	char msg[100];
	snprintf(msg, sizeof(msg), "(%zd of %zd found)", found, read);
    thread->stats.AddMessage(msg);
  }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->tid > 0) {
	    struct Stat0 ss;
		int fds[2];
		pid_t pid;
		pipe(fds);
		pid = fork();
		if (pid == 0) {
			ntdb_close(db_);
			db_ = 0;
			Open(dbflags_);
		  ReadRandom(thread);
		  ss.start = thread->stats.start_;
		  ss.finish = thread->stats.finish_;
		  ss.seconds = thread->stats.seconds_;
		  ss.done = thread->stats.done_;
		  ss.bytes = thread->stats.bytes_;
		  write(fds[1], &ss, sizeof(ss));
		} else {
		  waitpid(pid, NULL, 0);
		  read(fds[0], &ss, sizeof(ss));
		  thread->stats.start_ = ss.start;
		  thread->stats.finish_ = ss.finish;
		  thread->stats.seconds_ = ss.seconds;
		  thread->stats.done_ = ss.done;
		  thread->stats.bytes_ = ss.bytes;
		}
    } else {
	  BGWriter(thread);
	}
  }

  void BGWriter(ThreadState* thread) {
	// Special thread that keeps writing until other threads are done.
	RandomGenerator gen;
	double last = Env::Default()->NowMicros();
	int writes_per_second_by_10 = 0;
	int num_writes = 0;

	// --writes_per_second rate limit is enforced per 100 milliseconds
	// intervals to avoid a burst of writes at the start of each second.
	if (FLAGS_writes_per_second > 0)
		writes_per_second_by_10 = FLAGS_writes_per_second / 10;

	// Don't merge stats from this thread with the readers.
	thread->stats.SetExcludeFromMerge();

	while (true) {
	  {
		MutexLock l(&thread->shared->mu);
		if (thread->shared->num_done + 1 >= thread->shared->num_initialized) {
		  // Other threads have finished
		  // Report and wipe out our own stats
		  char buf[100];
		  snprintf(buf, sizeof(buf), "(desired %d ops/sec)",
			FLAGS_writes_per_second);
		  thread->stats.Stop();
		  thread->stats.AddMessage(buf);
		  thread->stats.Report(Slice("writer"));
		  thread->stats.Start(thread->tid);
		  break;
		}
	  }

	  TDB_DATA tkey, tdata;
	  char key[100];
	  const unsigned long k = thread->rand.Next() % FLAGS_num;
	  int rc, ikey;

	  tkey.dptr = (unsigned char *)key;
	  tkey.dsize = snprintf(key, sizeof(key), "%016lx", k);
	  tdata.dptr = (unsigned char *)gen.Generate(value_size_).data();
	  tdata.dsize = value_size_;
	  ntdb_transaction_start(db_);
	  rc = ntdb_store(db_, tkey, tdata, 0);
	  if (rc) {
		fprintf(stderr, "put error: %s\n", ntdb_errorstr((enum NTDB_ERROR)rc));
		exit(1);
	  }
	  rc = ntdb_transaction_commit(db_);
	  if (rc) {
		fprintf(stderr, "commit error: %s\n", ntdb_errorstr((enum NTDB_ERROR)rc));
		exit(1);
	  }
	  thread->stats.FinishedSingleOp();

	  ++num_writes;
	  if (writes_per_second_by_10 && num_writes >= writes_per_second_by_10) {
		double now = Env::Default()->NowMicros();
		double usecs_since_last = now - last;

		num_writes = 0;
		last = now;

		if (usecs_since_last < 100000.0) {
		  Env::Default()->SleepForMicroseconds(100000.0 - usecs_since_last);
		  last = Env::Default()->NowMicros();
		}
      }
    }
  }
};

}  // namespace leveldb

int main(int argc, char** argv) {
  std::string default_db_path;
  for (int i = 1; i < argc; i++) {
    double d;
    int n;
    char junk;
    if (leveldb::Slice(argv[i]).starts_with("--benchmarks=")) {
      FLAGS_benchmarks = argv[i] + strlen("--benchmarks=");
    } else if (sscanf(argv[i], "--compression_ratio=%lf%c", &d, &junk) == 1) {
      FLAGS_compression_ratio = d;
    } else if (sscanf(argv[i], "--histogram=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_histogram = n;
    } else if (sscanf(argv[i], "--use_existing_db=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_use_existing_db = n;
    } else if (sscanf(argv[i], "--hash_size=%d%c", &n, &junk) == 1) {
      FLAGS_hash_size = n;
    } else if (sscanf(argv[i], "--num=%d%c", &n, &junk) == 1) {
      FLAGS_num = n;
    } else if (sscanf(argv[i], "--batch=%d%c", &n, &junk) == 1) {
      FLAGS_batch = n;
    } else if (sscanf(argv[i], "--reads=%d%c", &n, &junk) == 1) {
      FLAGS_reads = n;
    } else if (sscanf(argv[i], "--threads=%d%c", &n, &junk) == 1) {
      FLAGS_threads = n;
    } else if (sscanf(argv[i], "--duration=%d%c", &n, &junk) == 1) {
      FLAGS_duration = n;
    } else if (sscanf(argv[i], "--stats_interval=%d%c", &n, &junk) == 1) {
      FLAGS_stats_interval = n;
    } else if (sscanf(argv[i], "--writes_per_second=%d%c", &n, &junk) == 1) {
      FLAGS_writes_per_second = n;
    } else if (sscanf(argv[i], "--value_size=%d%c", &n, &junk) == 1) {
      FLAGS_value_size = n;
    } else if (strncmp(argv[i], "--db=", 5) == 0) {
      FLAGS_db = argv[i] + 5;
    } else if (sscanf(argv[i], "--shuffle=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_shuffle = n;
    } else if (sscanf(argv[i], "--readahead=%d%c", &n, &junk) == 1 &&
               (n == 0 || n == 1)) {
      FLAGS_readahead = n;
    } else {
      fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      exit(1);
    }
  }

  // Choose a location for the test database if none given with --db=<path>
  if (FLAGS_db == NULL) {
      leveldb::Env::Default()->GetTestDirectory(&default_db_path);
      default_db_path += "/dbbench";
      FLAGS_db = default_db_path.c_str();
  }

  if (FLAGS_shuffle) {
	  shuff = (int *)malloc(FLAGS_num * sizeof(int));
	  for (int i=0; i<FLAGS_num; i++)
		shuff[i] = i;
  }

  seeds = (uint32_t *)malloc(FLAGS_threads * sizeof(uint32_t));
  for (int i=0; i<FLAGS_threads; i++)
  	seeds[i] = i + 1000;

  unsetenv("LD_LIBRARY_PATH");
  leveldb::Benchmark benchmark;
  benchmark.Run();
  return 0;
}