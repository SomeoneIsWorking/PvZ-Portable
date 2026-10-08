/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#ifndef __SCENARIORUN_H__
#define __SCENARIORUN_H__

#include <string>

class LawnApp;

// Runs a JSON scenario headless on the real Board and writes one JSON line per update.
// Opened by -scenario=<file> -scenario-out=<file>; see ScenarioRun.cpp for the schema.
class ScenarioRun
{
public:
	explicit ScenarioRun(LawnApp* theApp) : mApp(theApp) { }

	// Returns the process exit code.
	int Run(const std::string& theScenarioPath, const std::string& theOutPath);

private:
	LawnApp* mApp;
};

#endif
