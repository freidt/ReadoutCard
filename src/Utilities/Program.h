/// \file Program.h
/// \brief Definition of the Program class.
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#pragma once

#include <atomic>
#include <boost/program_options.hpp>
#include "Utilities/Common.h"
#include "Utilities/Options.h"
#include "Utilities/UtilsDescription.h"
#include "RORC/Exception.h"

namespace AliceO2 {
namespace Rorc {
namespace Utilities {

/// Helper class for making a RORC utility program. It handles:
/// - Creation of the options_descripotion object
/// - Creation of the variables_map object
/// - Help message
/// - Exceptions & error messages
/// - SIGINT signals
class Program
{
  public:

    Program();
    virtual ~Program();

    /// Execute the program using the given arguments
    int execute(int argc, char** argv);

    /// Has the SIGINT signal been given? (usually Ctrl-C)
    static bool isSigInt()
    {
      return sFlagSigInt;
    }

  protected:

    /// Should output be verbose
    bool isVerbose() const
    {
      return mVerbose;
    }

  private:

    /// Get the description of the program
    virtual UtilsDescription getDescription() = 0;

    /// Add the program's options
    virtual void addOptions(boost::program_options::options_description& optionsDescription) = 0;

    /// The main function of the program
    virtual void run(const boost::program_options::variables_map& variablesMap) = 0;

    static std::atomic<bool> sFlagSigInt;

    bool mVerbose;

    static void sigIntHandler(int);

    void printHelp (const boost::program_options::options_description& optionsDescription);
};

} // namespace Utilities
} // namespace Rorc
} // namespace AliceO2
