/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#ifndef __ENUMNAMES_H__
#define __ENUMNAMES_H__

#include <string>

// The source's own enum names for zombie, seed and game mode values, as the headless tools read and write them.
namespace EnumNames
{
// Each throws std::runtime_error naming theWhat for a name the table does not hold.
int ZombieByName(const std::string& theName, const char* theWhat);
int SeedByName(const std::string& theName, const char* theWhat);
int GameModeByName(const std::string& theName, const char* theWhat);

// Null for a value the table does not hold.
const char* ZombieName(int theValue);
const char* SeedName(int theValue);
const char* CoinName(int theValue);
const char* BackgroundName(int theValue);
const char* PlantRowName(int theValue);
}

#endif
