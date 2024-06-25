#include "mds/MDLog.h"
#include "mds/SegmentBoundary.h"
#include "mds/events/ENoOp.h"
#include "mds/events/ESegment.h"
#include "stubs/TestRank.h"

#include "gtest/gtest.h"
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <random>
#include <ranges>
#include "stubs/journal.h"

using std::vector;
using std::unique_ptr;
class MDLogTest: public ::testing::Test
{
protected:
  unique_ptr<TestRank> rank;
  unique_ptr<MDLog> log;
  std::default_random_engine e_rnd;

  void SetUp() override
  {
    rank.reset(new TestRank());
    log = make_log(rank.get());
    rank->md_log = log.get();
    log->create_logger();
    e_rnd.seed(std::chrono::system_clock::now().time_since_epoch().count());
    ASSERT_NO_FATAL_FAILURE(create());
    g_conf().set_val("mds_debug_zombie_log_segments", "false");
    apply_config();
  }

  unique_ptr<MDLog> make_log(MDSRankBase* rank) {
    void* mem = std::aligned_alloc(8, sizeof(MDLog));
    std::memset(mem, 0xa5, sizeof(MDLog));
    MDLog* log = new (mem) MDLog(rank);
    return unique_ptr<MDLog>(log);
  }

  void TearDown() override
  {
    std::unique_lock l(rank->get_lock());
    rank->my_info->state = MDSMap::DaemonState::STATE_STOPPING;
    rank->finisher->wait_for_empty();
    log->shutdown();
    rank->finisher->stop();
    rank->my_info->state = MDSMap::DaemonState::STATE_STOPPED;
  }

  void create() {
    std::promise<int> created_p;
  
    auto on_created = new MDSInternalContextWrapper(rank.get(), new LambdaContext([&](int r) {
      created_p.set_value(r);
    }));
  
    log->create(on_created);

    auto created = created_p.get_future();

    ASSERT_EQ(std::future_status::ready, created.wait_for(std::chrono::seconds(10)));
    ASSERT_EQ(0, created.get());
  }

  void apply_config() {
    static std::set<std::string> changed = {
      "mds_debug_subtrees",
      "mds_log_event_large_threshold",
      "mds_log_events_per_segment",
      "mds_log_major_segment_event_ratio",
      "mds_log_max_events",
      "mds_log_max_segments",
      // "mds_log_pause",
      "mds_log_skip_corrupt_events",
      "mds_log_skip_unbounded_events"
    };

    log->handle_conf_change(changed, *rank->get_mds_map());
  }

  unique_ptr<LogEvent> make_regular_event(int min_size = 11, int max_size = 117)
  {
    std::uniform_int_distribution<uint32_t> d_rnd(min_size, max_size);
    return unique_ptr<LogEvent>(new ENoOp(d_rnd(e_rnd)));
  }

  unique_ptr<LogEvent> make_boundary_event(bool major)
  {
    if (major) {
      return unique_ptr<LogEvent>(rank->get_cache_log_proxy()->create_subtree_map());
    } else {
      return unique_ptr<LogEvent>(new ESegment());
    }
  }

  vector<vector<unique_ptr<LogEvent>>>
  generate_random_segments(unsigned min_segments = 0, unsigned min_events = 5, unsigned max_events = 50, unsigned major_ratio = 5)
  {
    std::uniform_int_distribution<unsigned> d_rnd(min_events, max_events);

    unsigned segment_count = d_rnd(e_rnd);
    if (segment_count < min_segments) {
      segment_count = min_segments;
    }
    std::normal_distribution<> major_dist_rnd(segment_count / major_ratio);
    vector<vector<unique_ptr<LogEvent>>> segment_events;

    int next_major_in = 0;
    for (int s = 0; s < segment_count; s++) {
      segment_events.push_back({});
      vector<unique_ptr<LogEvent>>& events = segment_events.at(s);

      bool major = next_major_in == 0;
      events.push_back(make_boundary_event(major));
      if (major) {
        next_major_in = major_dist_rnd(e_rnd);
      } else {
        --next_major_in;
      }

      for (int e = d_rnd(e_rnd); e > 1 /* 1 for the segment boundary */; e--) {
        events.push_back(make_regular_event());
      }
    }

    return segment_events;
  }

  void flush_and_wait(double timeout = 10.0) {
    C_SaferCond * on_safe = new C_SaferCond();
    log->wait_for_safe(on_safe);
    log->flush();

    int result = on_safe->wait_for(timeout);
    if (result != 0) {
      FAIL();
    }
  }
};

TEST_F(MDLogTest, InitialConditions)
{
  ASSERT_EQ(0, log->get_num_events());
  ASSERT_EQ(0, log->get_num_segments());
  ASSERT_TRUE(log->empty());
  ASSERT_FALSE(log->have_any_segments());
  ASSERT_EQ(0, log->get_num_replayed_segments());

  ASSERT_EQ(log->get_write_pos(), log->get_read_pos());
  ASSERT_EQ(log->get_write_pos(), log->get_safe_pos());

  ASSERT_NO_FATAL_FAILURE(flush_and_wait());
};

TEST_F(MDLogTest, FlushAll)
{
  auto segment_events = generate_random_segments();
  int expected_event_total = 0;
  int expected_segment_total = 0;

  for (auto& events: segment_events) {
    std::lock_guard l(rank->get_lock());
    expected_event_total += events.size();
    expected_segment_total += 1;

    for (auto& event: events) {
      log->submit_entry(event.release());
    }

    ASSERT_EQ(expected_event_total, log->get_num_events());
    ASSERT_EQ(expected_segment_total, log->get_num_segments());
  }

  ASSERT_NO_FATAL_FAILURE(flush_and_wait());
};

TEST_F(MDLogTest, TrimAll)
{
  int segs_since_last_major = 0;
  for (auto& events : generate_random_segments()) {
    std::lock_guard l(rank->get_lock());

    segs_since_last_major++;
    if (auto sb = dynamic_cast<SegmentBoundary*>(events.front().get())) {
      if (sb->is_major_segment_boundary()) {
        segs_since_last_major = 0;
      }
    }
    for (auto& event : events) {
      log->submit_entry(event.release());
    }
  }
  ASSERT_NO_FATAL_FAILURE(flush_and_wait());

  {
    std::lock_guard l(rank->get_lock());
    log->trim_all();
  }

  rank->get_finisher()->wait_for_empty();

  // we expect that the last major segment and all that we've seen after
  // will stay untrimmed, but no less than one last segment
  int expected_seg_count = std::max(1, segs_since_last_major + 1);
  EXPECT_EQ(expected_seg_count, log->get_num_segments());
}

TEST_F(MDLogTest, TrimTwoSegments)
{
  for (auto& events : std::ranges::take_view(generate_random_segments(2), 2)) {
    std::lock_guard l(rank->get_lock());

    // we want two major segments
    bool first_event = true;
    for (auto& event : events) {
      if (first_event) {
        first_event = false;
        log->submit_entry(make_boundary_event(true).release());
      } else {
        log->submit_entry(event.release());
      }
    }
  }
  EXPECT_EQ(2, log->get_num_segments());
  ASSERT_NO_FATAL_FAILURE(flush_and_wait());

  vector<Context*> expirations;
  journal_log_segment_expiration_hook = [&](LogSegment&, MDSRankBase*,MDSGatherBuilder& gather,int) {
    expirations.push_back(gather.new_sub());
  };

  {
    std::lock_guard l(rank->get_lock());
    log->trim_all();
    // we shouldn't expire the last segment
    EXPECT_EQ(1, expirations.size());
    journal_log_segment_expiration_hook = {};
  }

  C_SaferCond* did_expire = new C_SaferCond();

  {
    std::lock_guard l(rank->get_lock());
    // we should be able to await expiration of the first out of the two segments
    EXPECT_TRUE(log->await_expiring_segments(did_expire));
    std::ranges::for_each(expirations, [](auto c) { c->complete(0); });
    log->trim_expired_segments();
  }

  EXPECT_EQ(0, did_expire->wait_for(std::chrono::seconds(10)));

  EXPECT_EQ(1, log->get_num_segments());

  {
    std::lock_guard l(rank->get_lock());
    C_NoopContext noop;
    ASSERT_FALSE(log->await_expiring_segments(&noop));
  }
}
