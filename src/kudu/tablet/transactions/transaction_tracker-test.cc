// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tablet/transactions/transaction_tracker.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>
#include <google/protobuf/message.h>  // IWYU pragma: keep
#include <gtest/gtest.h>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/tablet/transactions/transaction.h"
#include "kudu/tablet/transactions/transaction_driver.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"
#include "kudu/util/thread.h"

DECLARE_int64(tablet_transaction_memory_limit_mb);

METRIC_DECLARE_entity(tablet);

METRIC_DECLARE_gauge_uint64(all_transactions_inflight);
METRIC_DECLARE_gauge_uint64(write_transactions_inflight);
METRIC_DECLARE_gauge_uint64(alter_schema_transactions_inflight);
METRIC_DECLARE_counter(transaction_memory_pressure_rejections);
METRIC_DECLARE_counter(transaction_memory_limit_rejections);

using std::pair;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace kudu {
namespace tablet {

class TransactionTrackerTest : public KuduTest,
                               public ::testing::WithParamInterface<pair<int, int>> {
 public:
  class NoOpTransactionState : public TransactionState {
   public:
    NoOpTransactionState() : TransactionState(nullptr) {}
    const google::protobuf::Message* request() const override { return &req_; }
    std::string ToString() const override { return "NoOpTransactionState"; }
   private:
    consensus::ReplicateMsg req_;
  };
  class NoOpTransaction : public Transaction {
   public:
    explicit NoOpTransaction(NoOpTransactionState* state)
      : Transaction(consensus::LEADER, Transaction::WRITE_TXN),
        state_(state) {
    }

    void NewReplicateMsg(unique_ptr<consensus::ReplicateMsg>* replicate_msg) override {
      replicate_msg->reset(new consensus::ReplicateMsg());
    }
    TransactionState* state() override { return state_.get();  }
    const TransactionState* state() const override { return state_.get();  }

    Status Prepare() override { return Status::OK(); }
    Status Start() override { return Status::OK(); }
    Status Apply(unique_ptr<consensus::CommitMsg>* /* commit_msg */) override {
      return Status::OK();
    }
    std::string ToString() const override {
      return "NoOp";
    }
   private:
    unique_ptr<NoOpTransactionState> state_;
  };

  TransactionTrackerTest()
      : entity_(METRIC_ENTITY_tablet.Instantiate(&registry_, "test")) {
    tracker_.StartInstrumentation(entity_);
  }

  void RunTransactionsThread(CountDownLatch* finish_latch);

  Status AddDrivers(int num_drivers,
                    vector<scoped_refptr<TransactionDriver> >* drivers) {
    vector<scoped_refptr<TransactionDriver> > local_drivers;
    for (int i = 0; i < num_drivers; i++) {
      scoped_refptr<TransactionDriver> driver(
          new TransactionDriver(&tracker_,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr));
      unique_ptr<NoOpTransaction> tx(new NoOpTransaction(new NoOpTransactionState));
      RETURN_NOT_OK(driver->Init(std::move(tx), consensus::LEADER));
      local_drivers.push_back(driver);
    }

    for (const scoped_refptr<TransactionDriver>& d : local_drivers) {
      drivers->push_back(d);
    }
    return Status::OK();
  }

  MetricRegistry registry_;
  scoped_refptr<MetricEntity> entity_;
  TransactionTracker tracker_;
};

TEST_F(TransactionTrackerTest, TestGetPending) {
  ASSERT_EQ(0, tracker_.GetNumPendingForTests());
  vector<scoped_refptr<TransactionDriver> > drivers;
  ASSERT_OK(AddDrivers(1, &drivers));
  scoped_refptr<TransactionDriver> driver = drivers[0];
  ASSERT_EQ(1, tracker_.GetNumPendingForTests());

  vector<scoped_refptr<TransactionDriver> > pending_transactions;
  tracker_.GetPendingTransactions(&pending_transactions);
  ASSERT_EQ(1, pending_transactions.size());
  ASSERT_EQ(driver.get(), pending_transactions.front().get());

  // And mark the transaction as failed, which will cause it to unregister itself.
  driver->Abort(Status::Aborted(""));

  ASSERT_EQ(0, tracker_.GetNumPendingForTests());
}

// Thread which starts a bunch of transactions and later stops them all.
void TransactionTrackerTest::RunTransactionsThread(CountDownLatch* finish_latch) {
  const int kNumTransactions = 100;
  // Start a bunch of transactions.
  vector<scoped_refptr<TransactionDriver> > drivers;
  ASSERT_OK(AddDrivers(kNumTransactions, &drivers));

  // Wait for the main thread to tell us to proceed.
  finish_latch->Wait();

  // Sleep a tiny bit to give the main thread a chance to get into the
  // WaitForAllToFinish() call.
  SleepFor(MonoDelta::FromMilliseconds(1));

  // Finish all the transactions
  for (const scoped_refptr<TransactionDriver>& driver : drivers) {
    // And mark the transaction as failed, which will cause it to unregister itself.
    driver->Abort(Status::Aborted(""));
  }
}

// Regression test for KUDU-384 (thread safety issue with TestWaitForAllToFinish)
TEST_F(TransactionTrackerTest, TestWaitForAllToFinish) {
  CountDownLatch finish_latch(1);
  scoped_refptr<Thread> thr;
  CHECK_OK(Thread::Create("test", "txn-thread",
                          &TransactionTrackerTest::RunTransactionsThread, this, &finish_latch,
                          &thr));

  // Wait for the txns to start.
  while (tracker_.GetNumPendingForTests() == 0) {
    SleepFor(MonoDelta::FromMilliseconds(1));
  }

  // Allow the thread to proceed, and then wait for it to abort all the
  // transactions.
  finish_latch.CountDown();
  tracker_.WaitForAllToFinish();

  CHECK_OK(ThreadJoiner(thr.get()).Join());
  ASSERT_EQ(tracker_.GetNumPendingForTests(), 0);
}

static void CheckMetrics(const scoped_refptr<MetricEntity>& entity,
                         int expected_num_writes,
                         int expected_num_alters,
                         int expected_num_rejections,
                         int expected_num_rejections_for_limit) {
  ASSERT_EQ(expected_num_writes + expected_num_alters, down_cast<AtomicGauge<uint64_t>*>(
      entity->FindOrNull(METRIC_all_transactions_inflight).get())->value());
  ASSERT_EQ(expected_num_writes, down_cast<AtomicGauge<uint64_t>*>(
      entity->FindOrNull(METRIC_write_transactions_inflight).get())->value());
  ASSERT_EQ(expected_num_alters, down_cast<AtomicGauge<uint64_t>*>(
      entity->FindOrNull(METRIC_alter_schema_transactions_inflight).get())->value());
  ASSERT_EQ(expected_num_rejections, down_cast<Counter*>(
      entity->FindOrNull(METRIC_transaction_memory_pressure_rejections).get())->value());
  ASSERT_EQ(expected_num_rejections_for_limit, down_cast<Counter*>(
      entity->FindOrNull(METRIC_transaction_memory_limit_rejections).get())->value());
}

// Basic testing for metrics. Note that the NoOpTransactions we use in this
// test are all write transactions.
TEST_F(TransactionTrackerTest, TestMetrics) {
  NO_FATALS(CheckMetrics(entity_, 0, 0, 0, 0));

  vector<scoped_refptr<TransactionDriver> > drivers;
  ASSERT_OK(AddDrivers(3, &drivers));
  NO_FATALS(CheckMetrics(entity_, 3, 0, 0, 0));

  drivers[0]->Abort(Status::Aborted(""));
  NO_FATALS(CheckMetrics(entity_, 2, 0, 0, 0));

  drivers[1]->Abort(Status::Aborted(""));
  drivers[2]->Abort(Status::Aborted(""));
  NO_FATALS(CheckMetrics(entity_, 0, 0, 0, 0));
}

// Check that the tracker's consumption is very close (but not quite equal to)
// the passed transaction memory limit.
static void CheckMemTracker(const shared_ptr<MemTracker>& t, uint64_t limit_mb
     = FLAGS_tablet_transaction_memory_limit_mb) {
  int64_t val = t->consumption();
  uint64_t defined_limit = limit_mb * 1024 * 1024;
  ASSERT_GT(val, (defined_limit * 99) / 100);
  ASSERT_LE(val, defined_limit);
}

// Test that if too many transactions are added, eventually the tracker starts
// rejecting new ones.
TEST_P(TransactionTrackerTest, TestTooManyTransactions) {
  // First is the root tracker memory limit and the second is current tracker memory limit.
  const pair<int, int>& limit = GetParam();
  shared_ptr<MemTracker> t = MemTracker::CreateTracker(limit.first * 1024 * 1024, "test");
  // FLAGS_tablet_transaction_memory_limit_mb decides current memory tracker's limit.
  FLAGS_tablet_transaction_memory_limit_mb = limit.second;
  tracker_.StartMemoryTracking(t);

  // Fill up the tracker.
  //
  // It's difficult to anticipate exactly how many drivers we can add (each
  // carries an empty ReplicateMsg), so we'll just add as many as possible
  // and check that when we fail, it's because we've hit the limit.
  Status s;
  vector<scoped_refptr<TransactionDriver>> drivers;
  SCOPED_CLEANUP({
                   for (const auto &d : drivers) {
                     d->Abort(Status::Aborted(""));
                   }
                 });
  for (int i = 0; s.ok(); i++) {
    s = AddDrivers(1, &drivers);
  }

  LOG(INFO) << "Added " << drivers.size() << " drivers";
  int current_memory_limit_rejections_count = (limit.second <= limit.first) ? 1 : 0;
  int min_memory_limit = limit.first < limit.second ? limit.first : limit.second;
  ASSERT_TRUE(s.IsServiceUnavailable());
  ASSERT_STR_CONTAINS(s.ToString(), "exceeds the transaction memory limit");
  NO_FATALS(CheckMetrics(entity_, drivers.size(), 0, 1, current_memory_limit_rejections_count));
  NO_FATALS(CheckMemTracker(t, min_memory_limit));

  ASSERT_TRUE(AddDrivers(1, &drivers).IsServiceUnavailable());
  current_memory_limit_rejections_count += (limit.second <= limit.first) ? 1 : 0;
  NO_FATALS(CheckMetrics(entity_, drivers.size(), 0, 2, current_memory_limit_rejections_count));
  NO_FATALS(CheckMemTracker(t, min_memory_limit));

  // If we abort one transaction, we should be able to add one more.
  drivers.back()->Abort(Status::Aborted(""));
  drivers.pop_back();
  NO_FATALS(CheckMemTracker(t, min_memory_limit));
  ASSERT_OK(AddDrivers(1, &drivers));
  NO_FATALS(CheckMemTracker(t, min_memory_limit));
}

// Tests too many transactions with two memory tracker limits. First is the root tracker
// memory limit and the second is current tracker memory limit.
INSTANTIATE_TEST_CASE_P(MemoryLimitsMb, TransactionTrackerTest, ::testing::ValuesIn(
    vector<pair<int, int>> { {2, 1}, {1, 2}, {2, 2} }));

} // namespace tablet
} // namespace kudu
