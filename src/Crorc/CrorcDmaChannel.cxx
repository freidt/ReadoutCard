// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file CrorcDmaChannel.cxx
/// \brief Implementation of the CrorcDmaChannel class.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)
/// \author Kostas Alexopoulos (kostas.alexopoulos@cern.ch)

#include "CrorcDmaChannel.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <thread>
#include <boost/circular_buffer.hpp>
#include <boost/format.hpp>
#include "ChannelPaths.h"
#include "Crorc/Constants.h"
#include "ReadoutCard/ChannelFactory.h"
#include "Utilities/SmartPointer.h"

namespace b = boost;
//namespace bip = boost::interprocess;
//namespace bfs = boost::filesystem;
using std::cout;
using std::endl;
using namespace std::literals;

namespace AliceO2
{
namespace roc
{

CrorcDmaChannel::CrorcDmaChannel(const Parameters& parameters)
  : DmaChannelPdaBase(parameters, allowedChannels()),                          //
    mPageSize(parameters.getDmaPageSize().get_value_or(DMA_PAGE_SIZE)),        // 8 kB default for uniformity with CRU
    mInitialResetLevel(ResetLevel::Internal),                                  // It's good to reset at least the card channel in general
    mSTBRD(parameters.getStbrdEnabled().get_value_or(false)),                  //TODO: Set as a parameter
    mUseFeeAddress(false),                                                     // Not sure
    mDataSource(parameters.getDataSource().get_value_or(DataSource::Internal)) // Internal loopback by default
{
  // Check that the DMA page is valid
  if (mPageSize != DMA_PAGE_SIZE) {
    BOOST_THROW_EXCEPTION(CrorcException() << ErrorInfo::Message("CRORC only supports 8KiB DMA page size")
                                           << ErrorInfo::DmaPageSize(mPageSize));
  }

  // Check that the data source is valid. If not throw
  if (mDataSource == DataSource::Ddg) {
    BOOST_THROW_EXCEPTION(CruException() << ErrorInfo::Message("CRORC does not support specified data source")
                                         << ErrorInfo::DataSource(mDataSource));
  }

  mGeneratorEnabled = (mDataSource == DataSource::Fee) ? false : true;

  // Set mRDYRX if generator is disabled and mSTBRD is false
  if (!mGeneratorEnabled) {
    mRDYRX = mSTBRD ? false : true;
  }

  // Prep for BAR
  auto bar = ChannelFactory().getBar(parameters);
  crorcBar = std::move(std::dynamic_pointer_cast<CrorcBar>(bar)); // Initialize bar0

  // Create and register our ReadyFIFO buffer
  log("Initializing ReadyFIFO DMA buffer", InfoLogger::InfoLogger::Debug);
  {
    // Create and register the buffer
    // Note: if resizing the file fails, we might've accidentally put the file in a hugetlbfs mount with 1 GB page size
    constexpr auto FIFO_SIZE = sizeof(ReadyFifo);
    Utilities::resetSmartPtr(mBufferFifoFile, getPaths().fifo(), FIFO_SIZE, true);
    Utilities::resetSmartPtr(mPdaDmaBufferFifo, getRocPciDevice().getPciDevice(), mBufferFifoFile->getAddress(),
                             FIFO_SIZE, getPdaDmaBufferIndexFifo(getChannelNumber()), false); // note the 'false' at the end specifies non-hugepage memory

    const auto& entry = mPdaDmaBufferFifo->getScatterGatherList().at(0);
    if (entry.size < FIFO_SIZE) {
      // Something must've failed at some point
      BOOST_THROW_EXCEPTION(Exception()
                            << ErrorInfo::Message("Scatter gather list entry for internal FIFO was too small")
                            << ErrorInfo::ScatterGatherEntrySize(entry.size)
                            << ErrorInfo::FifoSize(FIFO_SIZE));
    }
    mReadyFifoAddressUser = entry.addressUser;
    mReadyFifoAddressBus = entry.addressBus;
  }

  getReadyFifoUser()->reset();
  mDmaBufferUserspace = getBufferProvider().getAddress();

  deviceResetChannel(mInitialResetLevel);
}

auto CrorcDmaChannel::allowedChannels() -> AllowedChannels
{
  return { 0, 1, 2, 3, 4, 5 };
}

CrorcDmaChannel::~CrorcDmaChannel()
{
  //deviceStopDma();
}

void CrorcDmaChannel::deviceStartDma()
{
  // Find DIU version, required for armDdl()
  mDiuConfig = getCrorc().initDiuVersion();

  // Arming the DDL, according to the channel parameters
  if ((mDataSource == DataSource::Siu) || (mDataSource == DataSource::Fee)) {
    armDdl(ResetLevel::InternalDiuSiu);
  } else if (mDataSource == DataSource::Diu) {
    armDdl(ResetLevel::InternalDiu);
  } else {
    armDdl(ResetLevel::Internal);
  }

  // Setting the card to be able to receive data
  startDataReceiving();

  log("DMA start deferred until enough superpages available");

  mFreeFifoFront = 0;
  mFreeFifoBack = 0;
  mFreeFifoSize = 0;
  mReadyQueue.clear();
  mTransferQueue.clear();
  mPendingDmaStart = true;
}

void CrorcDmaChannel::startPendingDma()
{
  if (!mPendingDmaStart) {
    return;
  }

  if (mTransferQueue.empty()) { // We should never end up in here
    log("Insufficient superpages to start pending DMA");
    return;
  }

  log("Starting pending DMA");

  if (mGeneratorEnabled) {
    log("Starting data generator");
    startDataGenerator();
  } else {
    if (mRDYRX || mSTBRD) {
      log("Starting trigger");

      // Clearing SIU/DIU status.
      getCrorc().assertLinkUp();
      getCrorc().siuCommand(Ddl::RandCIFST);
      getCrorc().diuCommand(Ddl::RandCIFST);

      uint32_t command = (mRDYRX) ? Fee::RDYRX : Fee::STBRD;

      // RDYRX command to FEE
      getCrorc().startTrigger(mDiuConfig, command);
    }
  }

  std::this_thread::sleep_for(100ms);

  mPendingDmaStart = false;
  log("DMA started");
}

void CrorcDmaChannel::deviceStopDma()
{
  if (mGeneratorEnabled) {
    getCrorc().stopDataGenerator();
  } else {
    if (mRDYRX || mSTBRD) {
      // Sending EOBTR to FEE.
      getCrorc().stopTrigger(mDiuConfig);
    }
  }
  getCrorc().stopDataReceiver();
}

void CrorcDmaChannel::deviceResetChannel(ResetLevel::type resetLevel)
{
  mDiuConfig = getCrorc().initDiuVersion();
  uint32_t command;
  StWord status;
  long long int timeout = Ddl::RESPONSE_TIME * mDiuConfig.pciLoopPerUsec;

  if (resetLevel == ResetLevel::Internal) {
    log("Resetting CRORC");
    log("Clearing Free FIFO");
    log("Clearing other FIFOS");
    log("Clearing CRORC's byte counters");
    command = Rorc::Reset::RORC | Rorc::Reset::FF | Rorc::Reset::FIFOS | Rorc::Reset::ERROR | Rorc::Reset::COUNTERS;
    getCrorc().resetCommand(command, mDiuConfig);
  } else if (resetLevel == ResetLevel::InternalDiu) {
    log("Resetting CRORC & DIU");
    command = Rorc::Reset::RORC | Rorc::Reset::DIU;
    getCrorc().resetCommand(command, mDiuConfig);
  } else if (resetLevel == ResetLevel::InternalDiuSiu) {
    log("Resetting SIU...");
    log("Switching off CRORC loopback");
    getCrorc().setLoopbackOff();
    std::this_thread::sleep_for(100ms);

    log("Resetting DIU");
    getCrorc().resetCommand(Rorc::Reset::DIU, mDiuConfig);
    std::this_thread::sleep_for(100ms);

    log("Resetting SIU");
    getCrorc().resetCommand(Rorc::Reset::SIU, mDiuConfig);
    std::this_thread::sleep_for(100ms);

    status = getCrorc().ddlReadDiu(0, timeout);
    if (((status.stw >> 15) & 0x7) == 0x6) {
      BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("SIU in no signal state (probably not connected), unable to reset SIU."));
    }

    status = getCrorc().ddlReadSiu(0, timeout);
    /*if (status.stw == -1) { // Comparing unsigned with -1?
      BOOST_THROW_EXCEPTION(Exception() << 
          ErrorInfo::Message("Error: Timeout - SIU not responding, unable to reset SIU."));
    }*/
  }
  log("Done!");
}

void CrorcDmaChannel::armDdl(ResetLevel::type resetLevel)
{
  if (resetLevel == ResetLevel::Nothing) {
    return;
  }

  try {
    getCrorc().resetCommand(Rorc::Reset::RORC, mDiuConfig);

    if (DataSource::isExternal(mDataSource) && (resetLevel != ResetLevel::Internal)) { // At least DIU
      getCrorc().armDdl(Rorc::Reset::DIU, mDiuConfig);

      if ((resetLevel == ResetLevel::InternalDiuSiu) && (mDataSource != DataSource::Diu)) //SIU & FEE
      {
        // Wait a little before SIU reset.
        std::this_thread::sleep_for(100ms); /// XXX Why???
        // Reset SIU.
        getCrorc().armDdl(Rorc::Reset::SIU, mDiuConfig);
        getCrorc().armDdl(Rorc::Reset::DIU, mDiuConfig);
      }

      getCrorc().armDdl(Rorc::Reset::RORC, mDiuConfig);
      std::this_thread::sleep_for(100ms);

      if ((resetLevel == ResetLevel::InternalDiuSiu) && (mDataSource != DataSource::Diu)) //SIU & FEE
      {
        getCrorc().assertLinkUp();
        getCrorc().siuCommand(Ddl::RandCIFST);
      }

      getCrorc().diuCommand(Ddl::RandCIFST);
      std::this_thread::sleep_for(100ms);
    }

    getCrorc().resetCommand(Rorc::Reset::FF, mDiuConfig);
    std::this_thread::sleep_for(100ms); /// XXX Give card some time to reset the FreeFIFO
    getCrorc().assertFreeFifoEmpty();
  } catch (Exception& e) {
    e << ErrorInfo::ResetLevel(resetLevel);
    e << ErrorInfo::DataSource(mDataSource);
    throw;
  }

  // Wait a little after reset.
  std::this_thread::sleep_for(100ms); /// XXX Why???
}

void CrorcDmaChannel::startDataGenerator()
{
  getCrorc().armDataGenerator(mPageSize); //TODO: To be simplified

  if (DataSource::Internal == mDataSource) {
    getCrorc().setLoopbackOn();
    std::this_thread::sleep_for(100ms); // XXX Why???
  }

  if (DataSource::Siu == mDataSource) {
    getCrorc().setSiuLoopback(mDiuConfig);
    std::this_thread::sleep_for(100ms); // XXX Why???
    getCrorc().assertLinkUp();
    getCrorc().siuCommand(Ddl::RandCIFST);
    getCrorc().diuCommand(Ddl::RandCIFST);
  }

  if (DataSource::Diu == mDataSource) {
    getCrorc().setDiuLoopback(mDiuConfig);
    std::this_thread::sleep_for(100ms);
    getCrorc().diuCommand(Ddl::RandCIFST);
  }

  getCrorc().startDataGenerator();
}

void CrorcDmaChannel::startDataReceiving()
{
  getCrorc().startDataReceiver(mReadyFifoAddressBus);
}

int CrorcDmaChannel::getTransferQueueAvailable()
{
  return TRANSFER_QUEUE_CAPACITY - mTransferQueue.size();
}

int CrorcDmaChannel::getReadyQueueSize()
{
  return mReadyQueue.size();
}

auto CrorcDmaChannel::getSuperpage() -> Superpage
{
  if (mReadyQueue.empty()) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not get superpage, ready queue was empty"));
  }
  return mReadyQueue.front();
}

void CrorcDmaChannel::pushSuperpage(Superpage superpage)
{
  checkSuperpage(superpage);

  if (mTransferQueue.size() >= TRANSFER_QUEUE_CAPACITY) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not push superpage, transfer queue was full"));
  }

  if (mFreeFifoSize >= MAX_SUPERPAGE_DESCRIPTORS) {
    BOOST_THROW_EXCEPTION(Exception()
                          << ErrorInfo::Message("Could not push superpage, firmware queue was full (this should never happen)"));
  }

  auto busAddress = getBusOffsetAddress(superpage.getOffset());
  pushFreeFifoPage(mFreeFifoFront, busAddress, superpage.getSize());
  mFreeFifoSize++;
  mFreeFifoFront = (mFreeFifoFront + 1) % MAX_SUPERPAGE_DESCRIPTORS;

  mTransferQueue.push_back(superpage);
}

auto CrorcDmaChannel::popSuperpage() -> Superpage
{
  if (mReadyQueue.empty()) {
    BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Could not pop superpage, ready queue was empty"));
  }
  auto superpage = mReadyQueue.front();
  mReadyQueue.pop_front();
  return superpage;
}

void CrorcDmaChannel::fillSuperpages()
{
  if (mPendingDmaStart) {
    if (!mTransferQueue.empty()) {
      startPendingDma();
    } else {
      // Waiting on enough superpages to start DMA...
      return;
    }
  }

  // Check for arrivals & handle them
  if (!mTransferQueue.empty()) { // i.e. If something is pushed to the CRORC
    auto isArrived = [&](int descriptorIndex) { return dataArrived(descriptorIndex) == DataArrivalStatus::WholeArrived; };
    auto resetDescriptor = [&](int descriptorIndex) { getReadyFifoUser()->entries[descriptorIndex].reset(); };
    auto getLength = [&](int descriptorIndex) { return getReadyFifoUser()->entries[descriptorIndex].length * 4; }; // length in 4B words

    while (mFreeFifoSize > 0) {
      if (isArrived(mFreeFifoBack)) {
        //size_t superpageFilled = SUPERPAGE_SIZE; // Get the length before updating our descriptor index
        size_t superpageFilled = getLength(mFreeFifoBack); // Get the length before updating our descriptor index
        resetDescriptor(mFreeFifoBack);

        mFreeFifoSize--;
        mFreeFifoBack = (mFreeFifoBack + 1) % MAX_SUPERPAGE_DESCRIPTORS;

        // Push Superpage
        auto superpage = mTransferQueue.front();
        superpage.setReceived(superpageFilled);
        superpage.setReady(true);
        mReadyQueue.push_back(superpage);
        mTransferQueue.pop_front();
      } else {
        // If the back one hasn't arrived yet, the next ones will certainly not have arrived either...
        break;
      }
    }
  }
}

// Return a boolean that denotes whether the transfer queue is empty
// The transfer queue is empty when all its slots are available
bool CrorcDmaChannel::isTransferQueueEmpty()
{
  return mTransferQueue.empty();
}

// Return a boolean that denotes whether the ready queue is full
// The ready queue is full when the CRORC has filled it up
bool CrorcDmaChannel::isReadyQueueFull()
{
  return mReadyQueue.size() == READY_QUEUE_CAPACITY;
}

int32_t CrorcDmaChannel::getDroppedPackets()
{
  log("No support for dropped packets in CRORC yet", InfoLogger::InfoLogger::Warning);
  return -1;
}

void CrorcDmaChannel::pushFreeFifoPage(int readyFifoIndex, uintptr_t pageBusAddress, int pageSize)
{
  size_t pageWords = pageSize / 4; // Size in 32-bit words
  getCrorc().pushRxFreeFifo(pageBusAddress, pageWords, readyFifoIndex);
}

CrorcDmaChannel::DataArrivalStatus::type CrorcDmaChannel::dataArrived(int index)
{
  auto length = getReadyFifoUser()->entries[index].length;
  auto status = getReadyFifoUser()->entries[index].status;

  if (status == -1) {
    return DataArrivalStatus::NoneArrived;
  } else if (status == 0) {
    return DataArrivalStatus::PartArrived;
  } else if ((status & 0xff) == Ddl::DTSW) {
    // Note: when internal loopback is used, the length of the event in words is also stored in the status word.
    // For example, the status word could be 0x400082 for events of size 4 kiB
    if ((status & (1 << 31)) != 0) {
      // The error bit is set
      BOOST_THROW_EXCEPTION(CrorcDataArrivalException()
                            << ErrorInfo::Message("Data arrival status word contains error bits")
                            << ErrorInfo::ReadyFifoStatus(status)
                            << ErrorInfo::ReadyFifoLength(length)
                            << ErrorInfo::FifoIndex(index));
    }
    return DataArrivalStatus::WholeArrived;
  }

  BOOST_THROW_EXCEPTION(CrorcDataArrivalException()
                        << ErrorInfo::Message("Unrecognized data arrival status word")
                        << ErrorInfo::ReadyFifoStatus(status)
                        << ErrorInfo::ReadyFifoLength(length)
                        << ErrorInfo::FifoIndex(index));
}

CardType::type CrorcDmaChannel::getCardType()
{
  return CardType::Crorc;
}

boost::optional<int32_t> CrorcDmaChannel::getSerial()
{
  return getBar()->getSerial();
}

boost::optional<std::string> CrorcDmaChannel::getFirmwareInfo()
{
  return getBar()->getFirmwareInfo();
}

} // namespace roc
} // namespace AliceO2
