#ifndef BROKER_PORT_HH
#define BROKER_PORT_HH

#include <cstdint>
#include <ostream>
#include <string>

#include "broker/util/operators.hh"

namespace broker {

/// A transport-layer port.
class port : util::totally_ordered<port> {
  template <class Processor>
  friend void serialize(Processor& proc, port& p, const unsigned int);

public:
  // Not using "using" because SWIG doesn't support it yet.
  typedef uint16_t number_type;

  enum class protocol : uint8_t {
    unknown,
    tcp,
    udp,
    icmp,
  };

  /// Default construct empty port, 0/unknown.
  port();

  /// Construct a port from number/protocol.
  port(number_type num, protocol p);

  /// @return The port number.
  number_type number() const;

  /// @return The port's transport protocol.
  protocol type() const;

  /// @return a string representation of the port argument.
  friend std::string to_string(const port& p);

  friend std::ostream& operator<<(std::ostream& out, const port& p);

  friend bool operator==(const port& lhs, const port& rhs);

  friend bool operator<(const port& lhs, const port& rhs);

private:
  number_type num;
  protocol proto;
};

std::string to_string(const port& p);
std::ostream& operator<<(std::ostream& out, const port& p);
bool operator==(const port& lhs, const port& rhs);
bool operator<(const port& lhs, const port& rhs);

template <class Processor>
void serialize(Processor& proc, port& p, const unsigned) {
  proc& p.num;
  proc& p.proto;
}

} // namespace broker

namespace std {
template <>
struct hash<broker::port> {
  size_t operator()(const broker::port&) const;
};
} // namespace std;

#endif // BROKER_PORT_HH
