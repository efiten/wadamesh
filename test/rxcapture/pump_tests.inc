// ---- RxPump ---------------------------------------------------------------
// Included by harness.cpp after FakeStore.

#include <vector>

struct FakeLink : RxLinkPort {
  bool is_up = true;
  bool busy = false;          // a publish is in flight
  bool ack = false, failure = false;
  bool refuse = false;        // publish() returns false
  std::vector<std::string> published;

  bool up() override { return is_up; }
  bool canPublish() override { return is_up && !busy; }
  bool publish(const char* line) override {
    if (refuse || !canPublish()) return false;
    published.push_back(line);
    busy = true;
    return true;
  }
  bool takeAck() override { if (!ack) return false; ack = false; busy = false; return true; }
  bool takeFailure() override { if (!failure) return false; failure = false; busy = false; return true; }
  void doAck() { ack = true; }
  void doFail() { failure = true; }
};

struct PumpRig {
  static const size_t kCap = 64;
  FakeLink link;
  FakeStore st;
  RxQueue q;
  char mem[4 * kCap];
  char scratch[kCap];
  RxPump pump;
  explicit PumpRig(uint8_t slots = 4) : q(st, 4096), pump(link, mem, slots, kCap, scratch) {
    q.begin();
    pump.setQueue(&q);
  }
};

static void test_pump_live_record_never_touches_the_file() {
  PumpRig r;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  CHECK(r.link.published.size() == 1);
  CHECK(r.link.published[0] == "p{\"n\":1}");
  r.link.doAck();
  r.pump.service();
  CHECK(r.pump.buffered() == 0);
  CHECK(r.st.data.empty());
  CHECK(r.q.pending() == 0);
}

static void test_pump_burst_waits_in_memory_and_keeps_order() {
  PumpRig r;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  r.pump.submit("p{\"n\":2}");
  r.pump.submit("r{\"n\":3}");
  r.pump.service();
  CHECK(r.st.data.empty());                 // one in flight, two waiting: all in memory
  CHECK(r.link.published.size() == 1);
  r.link.doAck(); r.pump.service();
  r.link.doAck(); r.pump.service();
  CHECK(r.link.published.size() == 3);
  CHECK(r.link.published[1] == "p{\"n\":2}");
  CHECK(r.link.published[2] == "r{\"n\":3}");
  r.link.doAck(); r.pump.service();
  CHECK(r.pump.buffered() == 0);
  CHECK(r.st.data.empty());
}

static void test_pump_failure_moves_everything_to_the_file_in_order() {
  PumpRig r;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  r.pump.submit("p{\"n\":2}");
  r.pump.submit("p{\"n\":3}");
  r.link.is_up = false;
  r.link.doFail();
  r.pump.service();
  CHECK(r.pump.buffered() == 0);
  CHECK(r.st.data == "p{\"n\":1}\np{\"n\":2}\np{\"n\":3}\n");
  CHECK(r.q.pending() == 3);
}

static void test_pump_link_down_goes_to_the_file() {
  PumpRig r;
  r.link.is_up = false;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  CHECK(r.link.published.empty());
  CHECK(r.pump.buffered() == 0);
  CHECK(r.q.pending() == 1);
}

static void test_pump_new_record_goes_behind_a_backlog() {
  PumpRig r;
  r.link.is_up = false;
  r.pump.submit("p{\"n\":1}");
  r.link.is_up = true;
  r.pump.submit("p{\"n\":2}");               // link is back, but an older record waits in the file
  CHECK(r.pump.buffered() == 0);
  CHECK(r.q.pending() == 2);
  r.pump.service();
  CHECK(r.link.published.size() == 1);
  CHECK(r.link.published[0] == "p{\"n\":1}");
  r.link.doAck(); r.pump.service();
  CHECK(r.link.published.size() == 2);
  CHECK(r.link.published[1] == "p{\"n\":2}");
  r.link.doAck(); r.pump.service();
  CHECK(r.q.pending() == 0);
}

static void test_pump_queued_record_stays_until_acked() {
  PumpRig r;
  r.link.is_up = false;
  r.pump.submit("p{\"n\":1}");
  r.link.is_up = true;
  r.pump.service();
  r.link.doFail();
  r.pump.service();                          // failed: still pending, and sent again
  CHECK(r.q.pending() == 1);
  CHECK(r.link.published.size() == 2);
  CHECK(r.link.published[1] == "p{\"n\":1}");
  r.link.doAck(); r.pump.service();
  CHECK(r.q.pending() == 0);
}

static void test_pump_full_memory_spills_the_waiting_records() {
  PumpRig r(2);
  r.pump.submit("p{\"n\":1}");
  r.pump.service();                          // 1 in flight
  r.pump.submit("p{\"n\":2}");
  r.pump.submit("p{\"n\":3}");               // memory full: 2 and 3 go to the file, 1 stays in flight
  CHECK(r.pump.buffered() == 1);
  CHECK(r.st.data == "p{\"n\":2}\np{\"n\":3}\n");
  r.link.doAck(); r.pump.service();
  CHECK(r.link.published.size() == 2);
  CHECK(r.link.published[1] == "p{\"n\":2}");
  CHECK(r.pump.lost() == 0);
}

static void test_pump_without_a_file_counts_what_it_cannot_keep() {
  PumpRig r(2);
  r.pump.setQueue(nullptr);
  r.link.is_up = false;
  r.pump.submit("p{\"n\":1}");
  r.pump.submit("p{\"n\":2}");
  r.pump.submit("p{\"n\":3}");
  CHECK(r.pump.buffered() == 2);
  CHECK(r.pump.lost() == 1);
  r.link.is_up = true;
  r.pump.service();
  CHECK(r.link.published.size() == 1);
  CHECK(r.link.published[0] == "p{\"n\":1}");
}

static void test_pump_shutdown_saves_what_is_unsent() {
  PumpRig r;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  r.pump.submit("p{\"n\":2}");
  r.link.is_up = false;
  r.link.doFail();                           // the session was stopped under the record in flight
  r.pump.shutdown();
  CHECK(r.pump.buffered() == 0);
  CHECK(r.st.data == "p{\"n\":1}\np{\"n\":2}\n");
}

static void test_pump_shutdown_keeps_an_acked_record_out_of_the_file() {
  PumpRig r;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  r.link.doAck();                            // the PUBACK arrived just before the stop
  r.pump.shutdown();
  CHECK(r.st.data.empty());
}

static void test_pump_refused_publish_is_tried_again() {
  PumpRig r;
  r.link.refuse = true;
  r.pump.submit("p{\"n\":1}");
  r.pump.service();
  CHECK(r.pump.buffered() == 1);
  r.link.refuse = false;
  r.pump.service();
  CHECK(r.link.published.size() == 1);
}

static void run_pump_tests() {
  test_pump_live_record_never_touches_the_file();
  test_pump_burst_waits_in_memory_and_keeps_order();
  test_pump_failure_moves_everything_to_the_file_in_order();
  test_pump_link_down_goes_to_the_file();
  test_pump_new_record_goes_behind_a_backlog();
  test_pump_queued_record_stays_until_acked();
  test_pump_full_memory_spills_the_waiting_records();
  test_pump_without_a_file_counts_what_it_cannot_keep();
  test_pump_shutdown_saves_what_is_unsent();
  test_pump_shutdown_keeps_an_acked_record_out_of_the_file();
  test_pump_refused_publish_is_tried_again();
}
