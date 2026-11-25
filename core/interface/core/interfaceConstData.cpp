#include "interfaceDefs.h"

constexpr uint8_t InterfaceGlobals::fxIDs[FX_COUNT];
constexpr char InterfaceGlobals::rndFxName[FX_NAME_SIZE + 4];
constexpr char InterfaceGlobals::fxNames[FX_COUNT][FX_NAME_SIZE + 4];
constexpr char InterfaceGlobals::fxShortNames[FX_COUNT][FX_NAME_SIZE + 4];
const char* InterfaceGlobals::ptrAllFxNames[FX_COUNT + 1];
const char* InterfaceGlobals::ptrAllFxNamesShort[FX_COUNT + 1];
const char* const* InterfaceGlobals::ptrFxNames;

namespace
{
struct FxNamePtrInit
{
    FxNamePtrInit()
    {
        InterfaceGlobals::ptrAllFxNames[0] = InterfaceGlobals::rndFxName;
        InterfaceGlobals::ptrAllFxNamesShort[0] = InterfaceGlobals::rndFxName;
        for (int i = 0; i < FX_COUNT; i++)
        {
            InterfaceGlobals::ptrAllFxNames[i + 1] = InterfaceGlobals::fxNames[i];
            InterfaceGlobals::ptrAllFxNamesShort[i + 1] = InterfaceGlobals::fxShortNames[i];
        }
        InterfaceGlobals::ptrFxNames = &InterfaceGlobals::ptrAllFxNames[1];
    }
} fxNamePtrInit;
}
