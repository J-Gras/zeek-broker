#pragma once

#include "broker/endpoint.hh"
#include "broker/internal/connector.hh"
#include "broker/internal/connector_adapter.hh"
#include "broker/internal/fwd.hh"

#include <caf/disposable.hpp>
#include <caf/flow/item_publisher.hpp>
#include <caf/flow/observable.hpp>
#include <caf/make_counted.hpp>
#include <caf/telemetry/counter.hpp>
#include <caf/telemetry/gauge.hpp>

#include <memory>

namespace broker::internal {

class peering : public std::enable_shared_from_this<peering> {
public:
  peering(endpoint_id id, endpoint_id peer_id) : id_(id), peer_id_(peer_id) {
    // nop
  }


  /// Called when the ACK message for out BYE.
  void on_bye_ack();

  /// Forces the peering to shut down its connection without performing the BYE
  /// handshake.
  void force_disconnect();

  void schedule_bye_timeout(caf::scheduled_actor* self);

  std::vector<std::byte> make_bye_token();

  node_message make_bye_message();

  /// Returns the status message after losing the connection. If the
  /// connection was closed by calling `remove`, this function returns a
  /// `peer_removed` message. Otherwise, `peer_disconnected`.
  node_message status_msg();

  /// Sets up the pipeline for this peer.
  caf::flow::observable<node_message>
  setup(caf::scheduled_actor* self, node_consumer_res in_res,
        node_producer_res out_res, caf::flow::observable<node_message> src);

  /// Queries whether `remove` was called.
  bool removed() const noexcept {
    return removed_;
  }

  /// Tag this peering as removed and send a BYE message on the `snk` for a
  /// graceful shutdown.
  void remove(caf::scheduled_actor* self,
              caf::flow::item_publisher<node_message>& snk,
              bool with_timeout = true);

  /// Returns the ID of this node.
  endpoint_id id() const {
    return id_;
  }

  /// Returns the ID of the peered node.
  endpoint_id peer_id() const {
    return peer_id_;
  }

  /// Returns the network address of the peered node.
  const network_info& addr() const {
    return addr_;
  }

  /// Sets a new value for the network address.
  void addr(const network_info& new_value) {
    addr_ = new_value;
  }

private:
  /// Indicates whether we have explicitly removed this connection by sending a
  /// BYE message to the peer.
  bool removed_ = false;

  /// Network address as reported from the transport (usually TCP).
  network_info addr_;

  /// Handle for aborting inputs.
  caf::disposable in_;

  /// Handle for aborting outputs.
  caf::disposable out_;

  /// Set in core_actor_state::unpeer.
  caf::disposable bye_timeout_;

  /// A 64-bit token that we use as ping payload when unpeering. The ping is
  /// the last message we send. When receiving a pong message with that token,
  /// we know all messages arrived and can shut down the connection.
  uint64_t bye_id_ = 0;

  /// The ID of this node.
  endpoint_id id_;

  /// The ID of our peer.
  endpoint_id peer_id_;
};

using peering_ptr = std::shared_ptr<peering>;

} // namespace broker::internal
