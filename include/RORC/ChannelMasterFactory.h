///
/// \file ChannelMasterFactory.h
/// \author Pascal Boeschoten
///

#pragma once

#include <memory>
#include "RORC/ChannelMasterInterface.h"
#include "RORC/ChannelParameters.h"

namespace AliceO2 {
namespace Rorc {

/// Factory class for creating objects to access RORC channels
class ChannelMasterFactory
{
  public:
    static constexpr int DUMMY_SERIAL_NUMBER = -1;

    ChannelMasterFactory();
    virtual ~ChannelMasterFactory();

    /// Get a channel object with the given serial number and channel number.
    /// It is not yet implemented fully, currently it will just pick the
    /// first CRORC it comes across. If the PDA dependency is not available, a dummy implementation will be returned.
    /// \param serialNumber The serial number of the card. Passing 'DUMMY_SERIAL_NUMBER' returns a dummy implementation
    std::shared_ptr<ChannelMasterInterface> getChannel(int serialNumber, int channelNumber, const ChannelParameters& params);
};

} // namespace Rorc
} // namespace AliceO2
