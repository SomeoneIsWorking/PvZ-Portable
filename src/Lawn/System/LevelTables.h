/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#ifndef __LEVELTABLES_H__
#define __LEVELTABLES_H__

#include <string>

class LawnApp;

// Writes what Board::InitLevel builds for every Adventure level and every challenge mode as one JSON
// document: lawn, rows, starting sun, waves and their zombies, the allowed roster, the seed bank and
// the award. Opened by -level-tables=<file>.
class LevelTables
{
public:
	explicit LevelTables(LawnApp* theApp) : mApp(theApp) { }

	// Returns the process exit code.
	int Write(const std::string& theOutPath);

private:
	LawnApp* mApp;
};

#endif
