/**
 * @file src/frame_warp_input.h
 * @brief What the input path tells Frame Warp, so src/input.cpp needs nothing else of it.
 *
 * The host counts the client's input where it is received on the control thread, before the input
 * queue batches or injects it (`input::passthrough`), so the sums the tags carry are those of the
 * packets the client sent, in the order it sent them. Only input that was accepted counts: a packet
 * refused by the client's permissions, or for a device the host's settings turned off, does not.
 */
#pragma once

// standard includes
#include <cstdint>

namespace frame_warp {

  class input_sink_t {
  public:
    virtual ~input_sink_t() = default;

    /// Frame Warp is on for this stream; one atomic load, so the input path pays nothing otherwise.
    virtual bool active() const = 0;

    /// One accepted relative mouse move (`NV_REL_MOUSE_MOVE_PACKET`), in counts.
    virtual void on_mouse(std::int16_t dx, std::int16_t dy) = 0;

    /// One accepted multi-controller packet and its right stick, for any controller number.
    virtual void on_controller(std::uint16_t number, std::int16_t right_stick_x, std::int16_t right_stick_y) = 0;
  };

}  // namespace frame_warp
