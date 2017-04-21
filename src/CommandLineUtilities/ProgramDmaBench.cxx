/// \file ProgramDmaBench.cxx
///
/// \brief Utility that tests RORC DMA performance
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)


#include <chrono>
#include <iomanip>
#include <iostream>
#include <future>
#include <fstream>
#include <random>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <boost/algorithm/string/trim.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/interprocess/sync/named_mutex.hpp>
#include <boost/format.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/lexical_cast.hpp>
#include "BarHammer.h"
#include "Cru/Constants.h"
#include "CommandLineUtilities/Common.h"
#include "CommandLineUtilities/Options.h"
#include "CommandLineUtilities/Program.h"
#include "Common/Iommu.h"
#include "Common/SuffixOption.h"
#include "ExceptionInternal.h"
#include "ExceptionLogging.h"
#include "InfoLogger/InfoLogger.hxx"
#include "folly/ProducerConsumerQueue.h"
#include "RORC/ChannelFactory.h"
#include "RORC/MemoryMappedFile.h"
#include "RORC/Parameters.h"
#include "Utilities/SmartPointer.h"
#include "Utilities/Util.h"

using namespace AliceO2::Rorc::CommandLineUtilities;
using namespace AliceO2::Rorc;
using namespace AliceO2::InfoLogger;
using AliceO2::Common::SuffixOption;
using std::cout;
using std::endl;
using namespace std::literals;
namespace b = boost;
namespace po = boost::program_options;

namespace {
/// Interval for low priority thread (display updates, etc)
constexpr auto LOW_PRIORITY_INTERVAL = 10ms;
/// Resting time if push thread has nothing to do
constexpr auto RESTING_TIME_PUSH_THREAD = 1us;
/// Resting time if readout thread has nothing to do
constexpr auto RESTING_TIME_READOUT_THREAD = 10us;
/// Buffer value to reset to
constexpr uint32_t BUFFER_DEFAULT_VALUE = 0xCcccCccc;
/// Fields: Time(hour:minute:second), Pages pushed, Pages read, Errors, °C
const std::string PROGRESS_FORMAT_HEADER("  %-8s   %-12s  %-12s  %-12s  %-5.1f");
/// Fields: Time(hour:minute:second), Pages pushed, Pages read, Errors, °C
const std::string PROGRESS_FORMAT("  %02s:%02s:%02s   %-12s  %-12s  %-12s  %-5.1f");
/// Path for error log
auto READOUT_ERRORS_PATH = "readout_errors.txt";
/// Max amount of errors that are recorded into the error stream
constexpr int64_t MAX_RECORDED_ERRORS = 1000;
/// End InfoLogger message alias
constexpr auto endm = InfoLogger::endm;
}

class ProgramDmaBench: public Program
{
  public:

    virtual Description getDescription()
    {
      return {"DMA Benchmark", "Test RORC DMA performance", "rorc-bench-dma --id=2:0.0 --channel=0 --reset --pages=1M "
        "--buffer-size=1Gi --verbose --superpage-size=128Ki"};
    }

    virtual void addOptions(po::options_description& options)
    {
      Options::addOptionChannel(options);
      Options::addOptionCardId(options);
      options.add_options()
          ("pages",
              SuffixOption<uint64_t>::make(&mOptions.maxPages)->default_value("10k"),
              "Amount of pages to transfer. Give 0 for infinite.")
          ("buffer-size",
              SuffixOption<size_t>::make(&mBufferSize)->default_value("10Mi"),
              "Buffer size in bytes. Rounded down to 2 MiB multiple. Minimum of 2 MiB. Use 2 MiB hugepage by default; |"
              "if buffer size is a multiple of 1 GiB, will try to use GiB hugepages")
          ("superpage-size",
              SuffixOption<size_t>::make(&mSuperpageSize)->default_value("1Mi"),
              "Superpage size in bytes. Note that it can't be larger than the buffer. If the IOMMU is not enabled, the "
              "hugepage size must be a multiple of the superpage size")
          ("reset",
              po::bool_switch(&mOptions.resetChannel),
              "Reset channel during initialization")
          ("to-file-ascii",
              po::bool_switch(&mOptions.fileOutputAscii),
              "Read out to file in ASCII format")
          ("to-file-bin",
              po::bool_switch(&mOptions.fileOutputBin),
              "Read out to file in binary format (only contains raw data from pages)")
          ("no-errorcheck",
              po::bool_switch(&mOptions.noErrorCheck),
              "Skip error checking")
          ("pattern",
              po::value<std::string>(&mOptions.generatorPatternString)->default_value("INCREMENTAL"),
              "Error check with given pattern [INCREMENTAL, ALTERNATING, CONSTANT, RANDOM]")
          ("readout-mode",
              po::value<std::string>(&mOptions.readoutModeString),
              "Set readout mode [CONTINUOUS]")
          ("no-resync",
              po::bool_switch(&mOptions.noResyncCounter),
              "Disable counter resync")
          ("page-reset",
              po::bool_switch(&mOptions.pageReset),
              "Reset page to default values after readout (slow)")
          ("bar-hammer",
              po::bool_switch(&mOptions.barHammer),
              "Stress the BAR with repeated writes and measure performance")
          ("random-pause",
              po::bool_switch(&mOptions.randomPause),
              "Randomly pause readout")
          ("rm-pages-file",
              po::bool_switch(&mOptions.removePagesFile),
              "Remove the file used for pages after benchmark completes");
      Options::addOptionsChannelParameters(options);
    }

    virtual void run(const po::variables_map& map)
    {
      auto cardId = Options::getOptionCardId(map);
      int channelNumber = Options::getOptionChannel(map);
      auto params = Options::getOptionsParameterMap(map);

      // Handle file output options
      if (mOptions.fileOutputAscii && mOptions.fileOutputBin) {
        throw ParameterException() << ErrorInfo::Message("File output can't be both ASCII and binary");
      } else {
        if (mOptions.fileOutputAscii) {
          mReadoutStream.open("readout_data.txt");
        }
        if (mOptions.fileOutputBin) {
          mReadoutStream.open("readout_data.bin", std::ios::binary);
        }
      }

      // Handle generator pattern option
      if (!mOptions.generatorPatternString.empty()) {
        mOptions.generatorPattern = GeneratorPattern::fromString(mOptions.generatorPatternString);
      }

      // Handle readout mode option
      if (!mOptions.readoutModeString.empty()) {
        mOptions.readoutMode = ReadoutMode::fromString(mOptions.readoutModeString);
      }

      // Create buffer
      {
        constexpr size_t SIZE_2MiB = 2*1024*1024;
        constexpr size_t SIZE_1GiB = 1*1024*1024*1024;
        HugePageType hugePageType;
        size_t hugePageSize;

        if (!Utilities::isMultiple(mBufferSize, SIZE_2MiB)) {
          throw ParameterException() << ErrorInfo::Message("Buffer size not a multiple of 2 MiB");
        }

        if (Utilities::isMultiple(mBufferSize, SIZE_1GiB)) {
          hugePageType = HugePageType::SIZE_1GB;
          hugePageSize = SIZE_1GiB;
        } else {
          hugePageType = HugePageType::SIZE_2MB;
          hugePageSize = SIZE_2MiB;
        }

        if (!AliceO2::Common::Iommu::isEnabled()) {
          if (!Utilities::isMultiple(hugePageSize, mSuperpageSize)) {
            throw ParameterException() << ErrorInfo::Message("IOMMU not enabled & hugepage size is not a multiple of "
                "superpage size. Superpages may cross hugepage boundaries and cause invalid PCIe memory accesses");
          }
          mLogger << "IOMMU not enabled" << endm;
        } else {
          mLogger << "IOMMU enabled" << endm;
        }

        if (mBufferSize < mSuperpageSize) {
          throw ParameterException() << ErrorInfo::Message("Buffer size smaller than superpage size");
        }

        auto createBuffer = [&](HugePageType hugePageType) {
          // Create buffer file
          std::string bufferFilePath = b::str(
              b::format("/var/lib/hugetlbfs/global/pagesize-%s/rorc-dma-bench_id=%s_chan=%s_pages")
                  % (hugePageType == HugePageType::SIZE_2MB ? "2MB" : "1GB") % map["id"].as<std::string>()
                  % channelNumber);
          Utilities::resetSmartPtr(mMemoryMappedFile, bufferFilePath, mBufferSize, mOptions.removePagesFile);
          mBufferBaseAddress = reinterpret_cast<uintptr_t>(mMemoryMappedFile->getAddress());
          mLogger << "Using buffer file path: " << bufferFilePath << endm;
        };

        if (hugePageType == HugePageType::SIZE_1GB) {
          try {
            createBuffer(HugePageType::SIZE_1GB);
          }
          catch (const MemoryMapException&) {
            mLogger << "Failed to allocate buffer with 1GiB hugepages, falling back to 2MiB hugepages" << endm;
          }
        }
        if (!mMemoryMappedFile) {
          createBuffer(HugePageType::SIZE_2MB);
        }
      }

      mInfinitePages = (mOptions.maxPages <= 0);

      // Set up channel parameters
      mPageSize = params.getDmaPageSize().get();
      params.setCardId(cardId);
      params.setChannelNumber(channelNumber);
      params.setGeneratorDataSize(mPageSize);
      params.setGeneratorPattern(mOptions.generatorPattern);
      params.setBufferParameters(BufferParameters::Memory { mMemoryMappedFile->getAddress(),
          mMemoryMappedFile->getSize() });
      if (mOptions.readoutMode) {
        params.setReadoutMode(*mOptions.readoutMode);
      }

      if (!Utilities::isMultiple(mSuperpageSize, mPageSize)) {
        throw ParameterException() << ErrorInfo::Message("Superpage size not a multiple of page size");
      }

      mMaxSuperpages = mBufferSize / mSuperpageSize;
      mPagesPerSuperpage = mSuperpageSize / mPageSize;
      mLogger << "Buffer size: " << mBufferSize << endm;
      mLogger << "Superpage size: " << mSuperpageSize << endm;
      mLogger << "Page size: " << mPageSize << endm;
      mLogger << "Max superpages: " << mMaxSuperpages << endm;
      mLogger << "Pages per superpage: " << mPagesPerSuperpage << endm;

      // Get master lock on channel
      try {
      mChannel = ChannelFactory().getMaster(params);
      }
      catch (const NamedMutexLockException& exception) {
        mLogger << InfoLogger::Warning << "Failed to acquire channel lock, attempting cleanup and retry" << endm;
        // Note that we can do this because we know for sure this process is not holding the lock. If we did not know
        // this, it would be very dangerous to manually remove the lock.
        if (auto mutexName = boost::get_error_info<ErrorInfo::NamedMutexName>(exception)) {
          boost::interprocess::named_mutex::remove(mutexName->c_str());
        }
        mChannel = ChannelFactory().getMaster(params);
      }
      mCardType = mChannel->getCardType();

      mLogger << "Card type: " << CardType::toString(mChannel->getCardType()) << endm;
      mLogger << "Firmware info: " << mChannel->getFirmwareInfo().value_or("unknown") << endm;

      if (mOptions.resetChannel) {
        mLogger << "Resetting channel" << endm;
        mChannel->resetChannel(ResetLevel::Rorc);
      }

      mLogger << "Starting benchmark" << endm;

      mChannel->startDma();

      if (mOptions.barHammer) {
        if (mChannel->getCardType() != CardType::Cru) {
          BOOST_THROW_EXCEPTION(ParameterException()
              << ErrorInfo::Message("BarHammer option currently only supported for CRU\n"));
        }
        Utilities::resetSmartPtr(mBarHammer);
        mBarHammer->start(mChannel);
      }

      mRunTime.start = std::chrono::steady_clock::now();
      dmaLoop();
      mRunTime.end = std::chrono::steady_clock::now();

      if (mBarHammer) {
        mBarHammer->join();
      }

      freeExcessPages(10ms);
      mChannel->stopDma();

      outputErrors();
      outputStats();

      mLogger << "Benchmark complete" << endm;
    }

  private:

    void dmaLoop()
    {
      auto indexToOffset = [&](int i) -> size_t { return i * mSuperpageSize; };

      if (mMaxSuperpages < 1) {
        throw std::runtime_error("Buffer too small");
      }

      // Lock-free queues. Usable size is (size-1), so we add 1
      folly::ProducerConsumerQueue<size_t> readoutQueue {mMaxSuperpages + 1};
      folly::ProducerConsumerQueue<size_t> freeQueue {mMaxSuperpages + 1};
      for (int i = 0; i < mMaxSuperpages; ++i) {
        if (!freeQueue.write(indexToOffset(i))) {
          BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Something went horribly wrong"));
        }
      }

      auto isStopDma = [&]{ return mDmaLoopBreak.load(std::memory_order_relaxed); };

      // Thread for low priority tasks
      auto lowPriorityFuture = std::async(std::launch::async, [&]{
        try {
          auto next = std::chrono::steady_clock::now();
          while (!isStopDma()) {
            lowPriorityTasks();
            next += LOW_PRIORITY_INTERVAL;
            std::this_thread::sleep_until(next);
          }
        }
        catch (std::exception& e) {
          mDmaLoopBreak = true;
          throw;
        }
      });

      // Thread for pushing & checking arrivals
      auto pushFuture = std::async(std::launch::async, [&]{
        try {
          RandomPauses pauses;
          int currentSuperpagePagesCounted = 0;
          auto rest = [&]{ std::this_thread::sleep_for(RESTING_TIME_PUSH_THREAD); };

          while (!isStopDma()) {
            // Check if we need to stop in the case of a page limit
            if (!mInfinitePages && mPushCount.load(std::memory_order_relaxed) >= mOptions.maxPages
                && (currentSuperpagePagesCounted == 0)) {
              break;
            }
            if (mOptions.randomPause) {
              pauses.pauseIfNeeded();
            }

            // Keep the driver's queue filled
            mChannel->fillSuperpages();

            // Give free superpages to the driver
            while (mChannel->getSuperpageQueueAvailable() != 0) {
              Superpage superpage;
              if (freeQueue.read(superpage.offset)) {
                superpage.size = mSuperpageSize;
                mChannel->pushSuperpage(superpage);
              } else {
                // No free pages available, so take a little break
                rest();
                break;
              }
            }

            // Check for filled superpages
            if (mChannel->getSuperpageQueueCount() > 0) {
              auto superpage = mChannel->getSuperpage();
              // We do partial updates of the mPushCount because we can have very large superpages, which would otherwise
              // cause hiccups in the display
              int pages = superpage.received / mPageSize;
              int pagesToCount = pages - currentSuperpagePagesCounted;
              mPushCount.fetch_add(pagesToCount, std::memory_order_relaxed);
              currentSuperpagePagesCounted += pagesToCount;

              if (superpage.isReady() && readoutQueue.write(superpage.getOffset())) {
                // Move full superpage to readout queue
                currentSuperpagePagesCounted = 0;
                mChannel->popSuperpage();
              } else {
                // Readout is backed up, so rest a while
                rest();
              }
            }
          }
        }
        catch (std::exception& e) {
          mDmaLoopBreak = true;
          throw;
        }
      });

      // Readout thread (main thread)
      {
        RandomPauses pauses;

        while (!isStopDma()) {
          if (!mInfinitePages && mReadoutCount.load(std::memory_order_relaxed) >= mOptions.maxPages) {
            mDmaLoopBreak = true;
            break;
          }
          if (mOptions.randomPause) {
            pauses.pauseIfNeeded();
          }
          auto rest = [&]{ std::this_thread::sleep_for(RESTING_TIME_READOUT_THREAD); };

          size_t offset;
          if (readoutQueue.read(offset)) {
            // Read out pages
            int pages = mSuperpageSize / mPageSize;
            for (int i = 0; i < pages; ++i) {
              auto readoutCount = mReadoutCount.fetch_add(1, std::memory_order_relaxed);
              readoutPage(mBufferBaseAddress + offset + i * mPageSize, mPageSize, readoutCount);
            }

            // Page has been read out
            // Add superpage back to free queue
            if (!freeQueue.write(offset)) {
              BOOST_THROW_EXCEPTION(Exception() << ErrorInfo::Message("Something went horribly wrong"));
            }
          } else {
            // No superpages available to read out, so have a nap
            rest();
          }
        }
      }

      pushFuture.get();
      lowPriorityFuture.get();
    }

    /// Free the pages that were pushed in excess
    void freeExcessPages(std::chrono::milliseconds timeout)
    {
      auto start = std::chrono::steady_clock::now();
      int popped = 0;
      while ((std::chrono::steady_clock::now() - start) < timeout) {
        if (mChannel->getSuperpageQueueCount() > 0) {
          auto superpage = mChannel->getSuperpage();
          if (superpage.isFilled()) {
            mChannel->popSuperpage();
            popped += superpage.getReceived() / mPageSize;
          }
        }
      }
      std::cout << "\n\n";
      mLogger << "Popped " << popped << " excess pages" << endm;
    }

    uint32_t getEventNumber(uintptr_t pageAddress)
    {
      auto page = reinterpret_cast<void*>(pageAddress);
      uint32_t eventNumber = 0;
      memcpy(&eventNumber, page, sizeof(eventNumber));
      return eventNumber;
    }

    void readoutPage(uintptr_t pageAddress, size_t pageSize, int64_t readoutCount)
    {
      // Read out to file
      if (mOptions.fileOutputAscii || mOptions.fileOutputBin) {
        printToFile(pageAddress, pageSize, readoutCount);
      }

      // Data error checking
      if (!mOptions.noErrorCheck) {
        auto getDataGeneratorCounterFromPage = [&]{
          switch (mCardType) {
            case CardType::Crorc:
              return getEventNumber(pageAddress);
              break;
            case CardType::Cru:
              return getEventNumber(pageAddress) / 256;
              break;
            default: throw std::runtime_error("Error checking unsupported for this card type");
          }
        };

        if (mDataGeneratorCounter == -1) {
          // First page initializes the counter
          mDataGeneratorCounter = getDataGeneratorCounterFromPage();
        }

        bool hasError = checkErrors(pageAddress, pageSize, readoutCount, mDataGeneratorCounter);
        if (hasError && !mOptions.noResyncCounter) {
          // Resync the counter
          mDataGeneratorCounter = getDataGeneratorCounterFromPage();
        }
      }

      if (mOptions.pageReset) {
        // Set the buffer to the default value after the readout
        resetPage(pageAddress, pageSize);
      }

      mDataGeneratorCounter++;
    }

    bool checkErrorsCru(uintptr_t pageAddress, size_t pageSize, int64_t eventNumber, uint32_t counter)
    {
      auto check = [&](auto patternFunction) {
        auto page = reinterpret_cast<const volatile uint32_t*>(pageAddress);
        auto pageSize32 = pageSize / sizeof(int32_t);
        constexpr uint32_t PATTERN_STRIDE = 8; // The data emulator writes to every 8th 32-bit word
        for (uint32_t i = 0; i < pageSize32; i += PATTERN_STRIDE)
        {
          uint32_t expectedValue = patternFunction(i);
          uint32_t actualValue = page[i];
          if (actualValue != expectedValue) {
            // Report error
            mErrorCount++;
            if (isVerbose() && mErrorCount < MAX_RECORDED_ERRORS) {
              mErrorStream << b::format("event:%d i:%d cnt:%d exp:0x%x val:0x%x\n")
                  % eventNumber % i % counter % expectedValue % actualValue;
            }
            return true;
          }
        }
        return false;
      };

      switch (mOptions.generatorPattern) {
        case GeneratorPattern::Incremental: return check([&](uint32_t i) { return counter * 256 + i / 8; });
        case GeneratorPattern::Alternating: return check([&](uint32_t)   { return 0xa5a5a5a5; });
        case GeneratorPattern::Constant:    return check([&](uint32_t)   { return 0x12345678; });
        default: ;
      }

      BOOST_THROW_EXCEPTION(Exception()
          << ErrorInfo::Message("Unsupported pattern for CRU error checking")
          << ErrorInfo::GeneratorPattern(mOptions.generatorPattern));
    }

    void addError(int64_t eventNumber, int index, uint32_t generatorCounter, uint32_t expectedValue,
        uint32_t actualValue)
    {
      mErrorCount++;
       if (isVerbose() && mErrorCount < MAX_RECORDED_ERRORS) {
         mErrorStream << b::format("event:%d i:%d cnt:%d exp:0x%x val:0x%x\n")
             % eventNumber % index % generatorCounter % expectedValue % actualValue;
       }
    }

    bool checkErrorsCrorc(uintptr_t pageAddress, size_t pageSize, int64_t eventNumber, uint32_t counter)
    {
      auto check = [&](auto patternFunction) {
        auto page = reinterpret_cast<const volatile uint32_t*>(pageAddress);
        auto pageSize32 = pageSize / sizeof(int32_t);

        if (page[0] != counter) {
          addError(eventNumber, 0, counter, counter, page[0]);
        }

        // We skip the SDH
        for (uint32_t i = 8; i < pageSize32; ++i) {
          uint32_t expectedValue = patternFunction(i);
          uint32_t actualValue = page[i];
          if (actualValue != expectedValue) {
            addError(eventNumber, i, counter, expectedValue, actualValue);
            return true;
          }
        }
        return false;
      };

      switch (mOptions.generatorPattern) {
        case GeneratorPattern::Incremental:
          return check([&](uint32_t i) {return i - 1;});
        case GeneratorPattern::Alternating:
          return check([&](uint32_t) {return 0xa5a5a5a5;});
        case GeneratorPattern::Constant:
          return check([&](uint32_t) {return 0x12345678;});
        default: ;
      }

      BOOST_THROW_EXCEPTION(Exception()
          << ErrorInfo::Message("Unsupported pattern for C-RORC error checking")
          << ErrorInfo::GeneratorPattern(mOptions.generatorPattern));
    }

    /// Checks and reports errors
    bool checkErrors(uintptr_t pageAddress, size_t pageSize, int64_t eventNumber, uint32_t counter)
    {
      switch (mCardType) {
        case CardType::Crorc:
          return checkErrorsCrorc(pageAddress, pageSize, eventNumber, counter);
        case CardType::Cru:
          return checkErrorsCru(pageAddress, pageSize, eventNumber, counter);
        default:
          throw std::runtime_error("Error checking unsupported for this card type");
      }
    }

    void resetPage(uintptr_t pageAddress, size_t pageSize)
    {
      auto page = reinterpret_cast<volatile uint32_t*>(pageAddress);
      auto pageSize32 = pageSize / sizeof(uint32_t);
      for (size_t i = 0; i < pageSize32; i++) {
        page[i] = BUFFER_DEFAULT_VALUE;
      }
    }

    void lowPriorityTasks()
    {
      // Handle a SIGINT abort
      if (isSigInt()) {
        // We want to finish the readout cleanly if possible, so we stop pushing and try to wait a bit until the
        // queue is empty
        mDmaLoopBreak = true;
        return;
      }

      // Status display updates
      // Wait until the DMA has really started before printing our table to avoid messy output
      if (isVerbose() && mPushCount.load(std::memory_order_relaxed) != 0) {
        updateStatusDisplay();
      }
    }

    void updateStatusDisplay()
    {
      if (!mHeaderPrinted) {
        printStatusHeader();
        mHeaderPrinted = true;
      }

       using namespace std::chrono;
       auto diff = steady_clock::now() - mRunTime.start;
       auto second = duration_cast<seconds>(diff).count() % 60;
       auto minute = duration_cast<minutes>(diff).count() % 60;
       auto hour = duration_cast<hours>(diff).count();

       auto format = b::format(PROGRESS_FORMAT);
       format % hour % minute % second; // Time
       format % mPushCount.load(std::memory_order_relaxed);
       format % mReadoutCount.load(std::memory_order_relaxed);

       mOptions.noErrorCheck ? format % "n/a" : format % mErrorCount; // Errors

       if (auto temperature = mChannel->getTemperature()) {
         format % *temperature;
       } else {
         format % "n/a";
       }

       cout << '\r' << format;

       // This takes care of adding a "line" to the stdout every so many seconds
       {
         constexpr int interval = 60;
         auto second = duration_cast<seconds>(diff).count() % interval;
         if (mDisplayUpdateNewline && second == 0) {
           cout << '\n';
           mDisplayUpdateNewline = false;
         }
         if (second >= 1) {
           mDisplayUpdateNewline = true;
         }
       }
     }

     void printStatusHeader()
     {
       auto line1 = b::format(PROGRESS_FORMAT_HEADER) % "Time" % "Pushed" % "Read" % "Errors" % "°C";
       auto line2 = b::format(PROGRESS_FORMAT) % "00" % "00" % "00" % '-' % '-' % '-' % '-';
       cout << '\n' << line1;
       cout << '\n' << line2;
     }

     void outputStats()
     {
       // Calculating throughput
       double runTime = std::chrono::duration<double>(mRunTime.end - mRunTime.start).count();
       double bytes = double(mReadoutCount.load()) * mPageSize;
       double GB = bytes / (1000 * 1000 * 1000);
       double GBs = GB / runTime;
       double Gbs = GBs * 8;

       auto put = [&](auto label, auto value) { cout << b::format("  %-10s  %-10s\n") % label % value; };
       cout << '\n';
       put("Seconds", runTime);
       put("Pages", mReadoutCount.load());
       if (bytes > 0.00001) {
         put("Bytes", bytes);
         put("GB", GB);
         put("GB/s", GBs);
         put("Gb/s", Gbs);
         if (mOptions.noErrorCheck) {
           put("Errors", "n/a");
         } else {
           put("Errors", mErrorCount);
         }
       }

       if (mOptions.barHammer) {
         size_t writeSize = sizeof(uint32_t);
         double hammerCount = mBarHammer->getCount();
         double bytes = hammerCount * writeSize;
         double MB = bytes / (1000 * 1000);
         double MBs = MB / runTime;
         put("BAR writes", hammerCount);
         put("BAR write size (bytes)", writeSize);
         put("BAR MB", MB);
         put("BAR MB/s", MBs);
       }

       cout << '\n';
     }

    void outputErrors()
    {
      auto errorStr = mErrorStream.str();

      if (isVerbose()) {
        size_t maxChars = 2000;
        if (!errorStr.empty()) {
          cout << "Errors:\n";
          cout << errorStr.substr(0, maxChars);
          if (errorStr.length() > maxChars) {
            cout << "\n... more follow (" << (errorStr.length() - maxChars) << " characters)\n";
          }
        }
      }

      std::ofstream stream(READOUT_ERRORS_PATH);
      stream << errorStr;
    }

    void printToFile(uintptr_t pageAddress, size_t pageSize, int64_t pageNumber)
    {
      auto page = reinterpret_cast<const volatile uint32_t*>(pageAddress);
      auto pageSize32 = pageSize / sizeof(int32_t);

      if (mOptions.fileOutputAscii) {
        mReadoutStream << "Event #" << pageNumber << '\n';
        int perLine = 8;

        for (int i = 0; i < pageSize32; i += perLine) {
          for (int j = 0; j < perLine; ++j) {
            mReadoutStream << page[i + j] << ' ';
          }
          mReadoutStream << '\n';
        }
        mReadoutStream << '\n';
      } else if (mOptions.fileOutputBin) {
        // TODO Is there a more elegant way to write from volatile memory?
        mReadoutStream.write(reinterpret_cast<const char*>(pageAddress), pageSize);
      }
    }

    using TimePoint = std::chrono::steady_clock::time_point;

    struct RandomPauses
    {
        static constexpr int NEXT_PAUSE_MIN = 10; ///< Minimum random pause interval in milliseconds
        static constexpr int NEXT_PAUSE_MAX = 2000; ///< Maximum random pause interval in milliseconds
        static constexpr int PAUSE_LENGTH_MIN = 1; ///< Minimum random pause in milliseconds
        static constexpr int PAUSE_LENGTH_MAX = 500; ///< Maximum random pause in milliseconds
        TimePoint next; ///< Next pause at this time
        std::chrono::milliseconds length; ///< Next pause has this length

        void pauseIfNeeded()
        {
          auto now = std::chrono::steady_clock::now();
          if (now >= next) {
            std::this_thread::sleep_for(length);
            // Schedule next pause
            auto now = std::chrono::steady_clock::now();
            next = now + std::chrono::milliseconds(Utilities::getRandRange(NEXT_PAUSE_MIN, NEXT_PAUSE_MAX));
            length = std::chrono::milliseconds(Utilities::getRandRange(PAUSE_LENGTH_MIN, PAUSE_LENGTH_MAX));
          }
        }
    };

    enum class HugePageType
    {
       SIZE_2MB, SIZE_1GB
    };

    /// Program options
    struct OptionsStruct {
        uint64_t maxPages = 0; ///< Limit of pages to push
        bool fileOutputAscii = false;
        bool fileOutputBin = false;
        bool resetChannel = false;
        bool randomPause = false;
        bool noErrorCheck = false;
        bool pageReset = false;
        bool noResyncCounter = false;
        bool barHammer = false;
        bool removePagesFile = false;
        bool delayReadout = false;
        std::string generatorPatternString;
        std::string readoutModeString;
        size_t superpageSizeMiB;
        GeneratorPattern::type generatorPattern = GeneratorPattern::Incremental;
        b::optional<ReadoutMode::type> readoutMode;
    } mOptions;

    std::atomic<bool> mDmaLoopBreak {false};
    bool mInfinitePages = false;
    std::atomic<int64_t> mPushCount { 0 };
    std::atomic<int64_t> mReadoutCount { 0 };
    int64_t mErrorCount = 0;
    int64_t mDataGeneratorCounter = -1;
    size_t mSuperpageSize = 0;
    size_t mMaxSuperpages = 0;
    size_t mPagesPerSuperpage = 0;

    std::unique_ptr<MemoryMappedFile> mMemoryMappedFile;

    /// Stream for file readout, only opened if enabled by the --file program options
    std::ofstream mReadoutStream;

    /// Stream for error output
    std::ostringstream mErrorStream;

    struct RunTime
    {
        TimePoint start; ///< Start of run time
        TimePoint end; ///< End of run time
    } mRunTime;

    /// Was the header printed?
    bool mHeaderPrinted = false;

    /// Time of the last display update
    TimePoint mLastDisplayUpdate;

    /// Indicates the display must add a newline to the table
    bool mDisplayUpdateNewline;

    size_t mPageSize;

    std::unique_ptr<BarHammer> mBarHammer;

    size_t mBufferSize;

    uintptr_t mBufferBaseAddress;

    CardType::type mCardType;

    InfoLogger mLogger;

    std::shared_ptr<ChannelMasterInterface> mChannel;
};

int main(int argc, char** argv)
{
  return ProgramDmaBench().execute(argc, argv);
}
