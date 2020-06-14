#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

#include <caf/actor.hpp>
#include <caf/meta/type_name.hpp>
#include <caf/send.hpp>

#include "broker/alm/lamport_timestamp.hh"
#include "broker/error.hh"

namespace broker::detail {

/// A message-driven channel for ensuring reliable and ordererd transport over
/// an unreliable and unordered communication layer. A channel belongs to a
/// single producer with any number of consumers.
template <class Handle, class Payload>
class channel {
public:
  // -- member types -----------------------------------------------------------

  /// Integer type for the monotonically increasing counters large enough to
  /// neglect wraparounds. At 1000 messages per second, a sequence number of
  /// this type overflows after 580 *million* years.
  using sequence_number_type = uint64_t;

  /// Integer type for measuring configurable intervals in ticks.
  using tick_interval_type = uint16_t;

  // -- member types: messages from consumers to the producer ------------------

  /// Notifies the producer that a consumer received all events up to a certain
  /// sequence number (including that number). Consumers send the latest ACK
  /// periodically as a keepalive message.
  struct cumulative_ack {
    sequence_number_type seq;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f,
                                                   cumulative_ack& x) {
      return f(caf::meta::type_name("cumulative_ack"), x.seq);
    }
  };

  /// Notifies the producer that a consumer failed to received some events.
  /// Sending a NACK for the sequence number 0 causes the publisher to re-send
  /// the handshake.
  struct nack {
    std::vector<sequence_number_type> seqs;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f, nack& x) {
      return f(caf::meta::type_name("nack"), x.seqs);
    }
  };

  // -- member types: messages from the producer to consumers ------------------

  /// Notifies a consumer which is the first sequence number after it started
  /// listening to the producer.
  struct handshake {
    /// The first sequence number a consumer should process and acknowledge.
    sequence_number_type first_seq;

    /// The interval (in ticks) between heartbeat messages. Allows the consumer
    /// to adjust its timeouts for detecting failed producers.
    tick_interval_type heartbeat_interval;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f, handshake& x) {
      return f(caf::meta::type_name("handshake"), x.first_seq,
               x.heartbeat_interval);
    }
  };

  /// Transmits ordered data to a consumer.
  struct event {
    sequence_number_type seq;
    Payload content;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f, event& x) {
      return f(caf::meta::type_name("event"), x.seq, x.content);
    }
  };

  /// Notifies a consumer that the producer can no longer retransmit an event.
  struct retransmit_failed {
    sequence_number_type seq;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f,
                                                   retransmit_failed& x) {
      return f(caf::meta::type_name("retransmit_failed"), x.seq);
    }
  };

  /// Notifies all consumers that the master is still alive and what is the
  /// latest sequence number.
  struct heartbeat {
    sequence_number_type seq;

    template <class Inspector>
    friend typename Inspector::result_type inspect(Inspector& f, heartbeat& x) {
      return f(caf::meta::type_name("heartbeat"), x.seq);
    }
  };

  // -- implementation of the producer -----------------------------------------

  /// Messages sent by the producer.
  using producer_message
    = caf::variant<handshake, event, retransmit_failed, heartbeat>;

  /// Produces events (messages) for any number of consumers.
  /// @tparam Backend Hides the underlying (unreliable) communication layer. The
  ///                 backend must provide the following member functions:
  ///                 - `void send(producer*, const Handle&, const T&)` sends a
  ///                   unicast message to a single consumer, where `T` is any
  ///                   type in `producer_message`.
  ///                 - `void broadcast(producer*, const T&)` sends a multicast
  ///                   message to all consumers, where `T` is any type in
  ///                   `producer_message`.
  template <class Backend>
  class producer {
  public:
    // -- member types ---------------------------------------------------------

    /// Bundles bookkeeping state for a consumer.
    struct path {
      /// Allows the backend to uniquely address this consumer.
      Handle hdl;

      /// The sequence number that was active when adding this consumer.
      sequence_number_type offset;

      /// The sequence number of the last cumulative ACK.
      sequence_number_type acked;

      /// The first time we have received a cumulative ACK for `acked`.
      alm::lamport_timestamp first_acked;

      /// The last time we have received a cumulative ACK for `acked`.
      alm::lamport_timestamp last_acked;
    };

    using buf_type = std::deque<event>;

    using path_list = std::vector<path>;

    // -- constructors, destructors, and assignment operators ------------------

    explicit producer(Backend* backend) : backend_(backend) {
      // nop
    }

    // -- message processing ---------------------------------------------------

    void produce(Payload content) {
      if (paths_.empty())
        return;
      ++seq_;
      buf_.emplace_back(event{seq_, std::move(content)});
      last_broadcast_ = tick_;
      backend_->broadcast(this, buf_.back());
    }

    error add(const Handle& hdl) {
      if (find_path(hdl) != paths_.end())
        return ec::consumer_exists;
      paths_.emplace_back(path{hdl, seq_, seq_});
      backend_->send(this, hdl, handshake{seq_, heartbeat_interval_});
      return nil;
    }

    void handle_ack(const Handle& hdl, sequence_number_type seq) {
      sequence_number_type acked = seq;
      // Iterate all paths once, fetching minimum acknowledged sequence number
      // and updating the path belonging to `hdl` in one go.
      for (auto& x : paths_) {
        if (x.hdl == hdl) {
          if (x.acked > seq) {
            // A blast from the past. Ignore.
            return;
          }
          if (x.acked == seq) {
            // Old news. Store the current time on the path and then stop
            // processing this event.
            x.last_acked = tick_;
            return;
          }
          x.acked = seq;
          x.first_acked = tick_;
          x.last_acked = tick_;
        } else {
          acked = std::min(x.acked, acked);
        }
      }
      // Drop events from the buffer if possible.
      auto not_acked = [acked](const event& x) { return x.seq > acked; };
      buf_.erase(buf_.begin(),
                 std::find_if(buf_.begin(), buf_.end(), not_acked));
    }

    void handle_nack(const Handle& hdl,
                     const std::vector<sequence_number_type>& seqs) {
      // Sanity checks.
      if (seqs.empty())
        return;
      auto p = find_path(hdl);
      if (p == paths_.end())
        return;
      // Seqs must be sorted. Everything before the first missing ID is ACKed.
      auto first = seqs.front();
      if (first == 0) {
        backend_->send(this, hdl, handshake{p->offset, heartbeat_interval_});
        return;
      }
      handle_ack(hdl, first - 1);
      for (auto seq : seqs) {
        if (auto i = find_event(seq); i != buf_.end())
          backend_->send(this, hdl, *i);
        else
          backend_->send(this, hdl, retransmit_failed{seq});
      }
    }

    // -- time-based processing ------------------------------------------------

    void tick() {
      ++tick_;
      if (heartbeat_interval_ > 0
          && last_broadcast_ + heartbeat_interval_ == tick_) {
        last_broadcast_ = tick_;
        backend_->broadcast(this, heartbeat{seq_});
      }
    }

    // -- properties -----------------------------------------------------------

    auto& backend() noexcept {
      return *backend_;
    }

    const auto& backend() const noexcept {
      return *backend_;
    }

    auto seq() const noexcept {
      return seq_;
    }

    const auto& buf() const noexcept {
      return buf_;
    }

    const auto& paths() const noexcept {
      return paths_;
    }

    auto heartbeat_interval() const noexcept {
      return heartbeat_interval_;
    }

    void heartbeat_interval(tick_interval_type value) noexcept {
      heartbeat_interval_ = value;
    }

    bool idle() const noexcept {
      auto at_head = [seq{seq_}](const path& x) { return x.acked == seq; };
      return std::all_of(paths_.begin(), paths_.end(), at_head);
    }

    // -- path and event lookup ------------------------------------------------

    auto find_path(const Handle& hdl) const noexcept {
      auto has_hdl = [&hdl](const path& x) { return x.hdl == hdl; };
      return std::find_if(paths_.begin(), paths_.end(), has_hdl);
    }

    auto find_event(sequence_number_type seq) const noexcept {
      auto has_seq = [seq](const event& x) { return x.seq == seq; };
      return std::find_if(buf_.begin(), buf_.end(), has_seq);
    }

  private:
    // -- member variables -----------------------------------------------------

    /// Transmits messages to the consumers.
    Backend* backend_;

    /// Monotonically increasing counter (starting at 1) to establish ordering
    /// of messages on this channel.
    sequence_number_type seq_ = 0;

    /// Monotonically increasing counter to keep track of time.
    alm::lamport_timestamp tick_;

    /// Stores the last time we've broadcasted something.
    alm::lamport_timestamp last_broadcast_;

    /// Stores outgoing events with their sequence number.
    buf_type buf_;

    /// List of consumers with the last acknowledged sequence number.
    path_list paths_;

    /// Maximum time between to broadcasted messages. When not sending anything
    /// else, insert heartbeats after this amount of time.
    tick_interval_type heartbeat_interval_ = 5;
  };

  // -- implementation of the consumer -----------------------------------------

  /// Messages sent by the consumer.
  using consumer_message = caf::variant<cumulative_ack, nack>;

  /// Handles events (messages) from a single producer.
  /// @tparam Backend Hides the underlying (unreliable) communication layer. The
  ///                 backend must provide the following member functions:
  ///                 - `void consume(consumer*, Payload)` process a single
  ///                 event.
  ///                 - `void send(consumer*, T)` sends a message to the
  ///                   producer, where `T` is any type in `consumer_message`.
  ///                 - `error consume_nil(consumer*)` process a lost event. The
  ///                   callback may abort further processing by returning a
  ///                   non-default `error`. In this case, the `consumer`
  ///                   immediately calls `close` with the returned error.
  ///                 - `void close(consumer*, error)` drops this consumer.
  ///                   After calling this function, no further function calls
  ///                   on the consumer are allowed (except calling the
  ///                   destructor).
  template <class Backend>
  class consumer {
  public:
    // -- member types ---------------------------------------------------------

    struct optional_event {
      sequence_number_type seq;
      optional<Payload> content;

      explicit optional_event(sequence_number_type seq) : seq(seq) {
        // nop
      }

      template <class T>
      optional_event(sequence_number_type seq, T&& x)
        : seq(seq), content(std::forward<T>(x)) {
        // nop
      }
    };

    using buf_type = std::deque<optional_event>;

    // -- constructors, destructors, and assignment operators ------------------

    explicit consumer(Backend* backend) : backend_(backend) {
      // nop
    }

    // -- message processing ---------------------------------------------------

    void handle_handshake(sequence_number_type offset,
                          tick_interval_type heartbeat_interval) {
      if (offset >= next_seq_) {
        next_seq_ = offset + 1;
        last_seq_ = next_seq_;
        heartbeat_interval_ = heartbeat_interval;
        try_consume_buffer();
      }
    }

    void handle_heartbeat(sequence_number_type seq) {
      // Do nothing when receiving this before the handshake or if the master
      // did not produce any events yet.
      if (last_seq_ == 0 || seq == 0)
        return;
      if (seq + 1 > last_seq_)
        last_seq_ = seq + 1;
    }

    void handle_event(sequence_number_type seq, Payload payload) {
      if (next_seq_ == seq) {
        // Process immediately.
        backend_->consume(this, std::move(payload));
        bump_seq();
        try_consume_buffer();
      } else if (seq > next_seq_) {
        if (seq > last_seq_)
          last_seq_ = seq;
        // Insert event into buf_: sort by the sequence number, drop duplicates.
        auto pred = [seq](const optional_event& x) { return x.seq >= seq; };
        auto i = std::find_if(buf_.begin(), buf_.end(), pred);
        if (i == buf_.end())
          buf_.emplace_back(seq, std::move(payload));
        else if (i->seq != seq)
          buf_.emplace(i, seq, std::move(payload));
        else if (!i->content)
          i->content = std::move(payload);
      }
    }

    void handle_retransmit_failed(sequence_number_type seq) {
      if (next_seq_ == seq) {
        // Process immediately.
        if (auto err = backend_->consume_nil(this)) {
          backend_->close(this, std::move(err));
          return;
        }
        bump_seq();
        try_consume_buffer();
      } else if (seq > next_seq_) {
        // Insert event into buf_: sort by the sequence number, drop duplicates.
        auto pred = [seq](const optional_event& x) { return x.seq >= seq; };
        auto i = std::find_if(buf_.begin(), buf_.end(), pred);
        if (i == buf_.end())
          buf_.emplace_back(seq);
        else if (i->seq != seq)
          buf_.emplace(i, seq);
      }
    }

    // -- time-based processing ------------------------------------------------

    void tick() {
      // Update state.
      bool progressed = next_seq_ > last_tick_seq_;
      last_tick_seq_ = next_seq_;
      ++tick_;
      if (progressed) {
        if (idle_ticks_ > 0)
          idle_ticks_ = 0;
        if (heartbeat_interval_ > 0 && num_ticks() % heartbeat_interval_ == 0)
          send_ack();
        return;
      }
      ++idle_ticks_;
      if (next_seq_ < last_seq_ && idle_ticks_ >= nack_timeout_) {
        idle_ticks_ = 0;
        auto first = next_seq_;
        auto last = last_seq_;
        std::vector<sequence_number_type> seqs;
        seqs.reserve(last - first);
        auto generate = [&, i{first}](sequence_number_type found) mutable {
          for (; i < found; ++i)
            seqs.emplace_back(i);
          ++i;
        };
        for (const auto& x : buf_)
          generate(x.seq);
        generate(last);
        backend_->send(this, nack{std::move(seqs)});
        return;
      }
      if (heartbeat_interval_ > 0 && num_ticks() % heartbeat_interval_ == 0)
        send_ack();
    }

    // -- properties -----------------------------------------------------------

    auto& backend() noexcept {
      return *backend_;
    }

    const auto& backend() const noexcept {
      return *backend_;
    }

    const buf_type& buf() const noexcept {
      return buf_;
    }

    auto num_ticks() const noexcept {
      // Lamport timestamps start at 1.
      return tick_.value - 1;
    }

    auto idle_ticks() const noexcept {
      return idle_ticks_;
    }

    auto heartbeat_interval() const noexcept {
      return heartbeat_interval_;
    }

    auto nack_timeout() const noexcept {
      return nack_timeout_;
    }

    void nack_timeout(uint8_t value) noexcept {
      nack_timeout_ = value;
    }

  private:
    // -- helper functions -----------------------------------------------------

    // Bumps the sequence number for the next expected event.
    void bump_seq() {
      if (++next_seq_ > last_seq_)
        last_seq_ = next_seq_;
    }

    // Consumes all events from buf_ until either hitting the end or hitting a
    // gap (i.e. events that are neither available yet nor known missing).
    void try_consume_buffer() {
      auto i = buf_.begin();
      for (; i != buf_.end() && i->seq == next_seq_; ++i) {
        if (i->content) {
          backend_->consume(this, *i->content);
        } else {
          if (auto err = backend_->consume_nil(this)) {
            buf_.erase(buf_.begin(), i);
            backend_->close(this, std::move(err));
            return;
          }
        }
        bump_seq();
      }
      buf_.erase(buf_.begin(), i);
    }

    void send_ack() {
      backend_->send(this, cumulative_ack{next_seq_ > 0 ? next_seq_ - 1 : 0});
    }

    // -- member variables -----------------------------------------------------

    /// Handles incoming events.
    Backend* backend_;

    /// Monotonically increasing counter (starting at 1) to establish ordering
    /// of messages on this channel.
    sequence_number_type next_seq_ = 0;

    /// The currently known end of the event stream.
    sequence_number_type last_seq_ = 0;

    /// Stores outgoing events with their sequence number.
    buf_type buf_;

    /// Monotonically increasing counter to keep track of time.
    alm::lamport_timestamp tick_;

    /// Stores the value of `next_seq_` at our last tick.
    sequence_number_type last_tick_seq_ = 0;

    /// Number of ticks without progress.
    uint8_t idle_ticks_ = 0;

    /// Frequency of ACK messages (configured by the master).
    uint8_t heartbeat_interval_ = 0;

    /// Number of ticks without progress before sending a NACK.
    uint8_t nack_timeout_ = 5;
  };
};

} // namespace broker::detail
