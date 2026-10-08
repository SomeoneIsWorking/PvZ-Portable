/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

// Headless value-level scenario: one zombie, optional pre-placed plants, N Board updates, one JSON
// line per update. Scenario file (all rows and columns 0-based):
//   { "lawn": "day", "zombie": "ZOMBIE_FOOTBALL", "row": 2, "startColumn": 8.5,
//     "plants": [ { "seed": "SEED_PEASHOOTER", "col": 0, "row": 2 } ],
//     "companions": [ { "zombie": "ZOMBIE_NORMAL", "row": 0 } ], "seconds": 30 }
// The first zombie of every line is the tracked one; companions are spawned after it.
// "startColumn" is optional: the zombie's mPosX is then LAWN_XMIN + 80 * startColumn instead of the
// spawn point. A line is { "tick": n, "zombies": [...], "plants": [...] }; tick 0 is before any update.

#include "ScenarioRun.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "../../LawnApp.h"
#include "../../GameConstants.h"
#include "../../ConstEnums.h"
#include "../../PvzpLib/Reanimator.h"
#include "../Board.h"
#include "../Plant.h"
#include "../Zombie.h"
#include "../LawnMower.h"
#include "PlayerInfo.h"

using namespace Sexy;

namespace
{

struct Json
{
	enum Kind { Null, Number, String, Array, Object } mKind = Null;
	double mNumber = 0;
	std::string mString;
	std::vector<Json> mItems;
	std::vector<std::string> mKeys;
	std::vector<Json> mValues;

	const Json* Find(const char* theKey) const
	{
		for (size_t i = 0; i < mKeys.size(); i++)
		{
			if (mKeys[i] == theKey)
				return &mValues[i];
		}
		return nullptr;
	}
};

class JsonReader
{
public:
	explicit JsonReader(const std::string& theText) : mText(theText) { }

	Json Read()
	{
		Json aValue = ReadValue();
		SkipSpace();
		if (mPos != mText.size())
			Fail("trailing text");
		return aValue;
	}

private:
	const std::string& mText;
	size_t mPos = 0;

	[[noreturn]] void Fail(const char* theWhat) const
	{
		throw std::runtime_error(std::string("scenario JSON: ") + theWhat + " at byte " + std::to_string(mPos));
	}

	void SkipSpace()
	{
		while (mPos < mText.size() && std::isspace(static_cast<unsigned char>(mText[mPos])))
			mPos++;
	}

	void Expect(char theChar)
	{
		SkipSpace();
		if (mPos >= mText.size() || mText[mPos] != theChar)
			Fail("unexpected character");
		mPos++;
	}

	std::string ReadString()
	{
		Expect('"');
		std::string aResult;
		while (mPos < mText.size() && mText[mPos] != '"')
		{
			if (mText[mPos] == '\\' && mPos + 1 < mText.size())
				mPos++;
			aResult += mText[mPos++];
		}
		if (mPos >= mText.size())
			Fail("unterminated string");
		mPos++;
		return aResult;
	}

	Json ReadValue()
	{
		SkipSpace();
		if (mPos >= mText.size())
			Fail("unexpected end");
		Json aValue;
		char aChar = mText[mPos];
		if (aChar == '{')
		{
			aValue.mKind = Json::Object;
			mPos++;
			SkipSpace();
			if (mText[mPos] == '}')
			{
				mPos++;
				return aValue;
			}
			for (;;)
			{
				SkipSpace();
				std::string aKey = ReadString();
				Expect(':');
				aValue.mKeys.push_back(aKey);
				aValue.mValues.push_back(ReadValue());
				SkipSpace();
				if (mText[mPos] == ',')
				{
					mPos++;
					continue;
				}
				Expect('}');
				return aValue;
			}
		}
		if (aChar == '[')
		{
			aValue.mKind = Json::Array;
			mPos++;
			SkipSpace();
			if (mText[mPos] == ']')
			{
				mPos++;
				return aValue;
			}
			for (;;)
			{
				aValue.mItems.push_back(ReadValue());
				SkipSpace();
				if (mText[mPos] == ',')
				{
					mPos++;
					continue;
				}
				Expect(']');
				return aValue;
			}
		}
		if (aChar == '"')
		{
			aValue.mKind = Json::String;
			aValue.mString = ReadString();
			return aValue;
		}
		if (mText.compare(mPos, 4, "null") == 0)
		{
			mPos += 4;
			return aValue;
		}
		char* anEnd = nullptr;
		aValue.mNumber = std::strtod(mText.c_str() + mPos, &anEnd);
		if (anEnd == mText.c_str() + mPos)
			Fail("expected a value");
		aValue.mKind = Json::Number;
		mPos = static_cast<size_t>(anEnd - mText.c_str());
		return aValue;
	}
};

struct NamedValue
{
	const char* mName;
	int mValue;
};

#define NAMED(theName) { #theName, theName }

const NamedValue gZombieNames[] = {
	NAMED(ZOMBIE_NORMAL), NAMED(ZOMBIE_FLAG), NAMED(ZOMBIE_TRAFFIC_CONE), NAMED(ZOMBIE_POLEVAULTER),
	NAMED(ZOMBIE_PAIL), NAMED(ZOMBIE_NEWSPAPER), NAMED(ZOMBIE_DOOR), NAMED(ZOMBIE_FOOTBALL),
	NAMED(ZOMBIE_DANCER), NAMED(ZOMBIE_BACKUP_DANCER), NAMED(ZOMBIE_DUCKY_TUBE), NAMED(ZOMBIE_SNORKEL),
	NAMED(ZOMBIE_ZAMBONI), NAMED(ZOMBIE_BOBSLED), NAMED(ZOMBIE_DOLPHIN_RIDER), NAMED(ZOMBIE_JACK_IN_THE_BOX),
	NAMED(ZOMBIE_BALLOON), NAMED(ZOMBIE_DIGGER), NAMED(ZOMBIE_POGO), NAMED(ZOMBIE_YETI),
	NAMED(ZOMBIE_BUNGEE), NAMED(ZOMBIE_LADDER), NAMED(ZOMBIE_CATAPULT), NAMED(ZOMBIE_GARGANTUAR),
	NAMED(ZOMBIE_IMP), NAMED(ZOMBIE_BOSS), NAMED(ZOMBIE_PEA_HEAD), NAMED(ZOMBIE_WALLNUT_HEAD),
	NAMED(ZOMBIE_JALAPENO_HEAD), NAMED(ZOMBIE_GATLING_HEAD), NAMED(ZOMBIE_SQUASH_HEAD),
	NAMED(ZOMBIE_TALLNUT_HEAD), NAMED(ZOMBIE_REDEYE_GARGANTUAR),
};

const NamedValue gSeedNames[] = {
	NAMED(SEED_PEASHOOTER), NAMED(SEED_SUNFLOWER), NAMED(SEED_CHERRYBOMB), NAMED(SEED_WALLNUT),
	NAMED(SEED_POTATOMINE), NAMED(SEED_SNOWPEA), NAMED(SEED_CHOMPER), NAMED(SEED_REPEATER),
	NAMED(SEED_PUFFSHROOM), NAMED(SEED_SUNSHROOM), NAMED(SEED_FUMESHROOM), NAMED(SEED_GRAVEBUSTER),
	NAMED(SEED_HYPNOSHROOM), NAMED(SEED_SCAREDYSHROOM), NAMED(SEED_ICESHROOM), NAMED(SEED_DOOMSHROOM),
	NAMED(SEED_LILYPAD), NAMED(SEED_SQUASH), NAMED(SEED_THREEPEATER), NAMED(SEED_TANGLEKELP),
	NAMED(SEED_JALAPENO), NAMED(SEED_SPIKEWEED), NAMED(SEED_TORCHWOOD), NAMED(SEED_TALLNUT),
	NAMED(SEED_SEASHROOM), NAMED(SEED_PLANTERN), NAMED(SEED_CACTUS), NAMED(SEED_BLOVER),
	NAMED(SEED_SPLITPEA), NAMED(SEED_STARFRUIT), NAMED(SEED_PUMPKINSHELL), NAMED(SEED_MAGNETSHROOM),
	NAMED(SEED_CABBAGEPULT), NAMED(SEED_FLOWERPOT), NAMED(SEED_KERNELPULT), NAMED(SEED_INSTANT_COFFEE),
	NAMED(SEED_GARLIC), NAMED(SEED_UMBRELLA), NAMED(SEED_MARIGOLD), NAMED(SEED_MELONPULT),
	NAMED(SEED_GATLINGPEA), NAMED(SEED_TWINSUNFLOWER), NAMED(SEED_GLOOMSHROOM), NAMED(SEED_CATTAIL),
	NAMED(SEED_WINTERMELON), NAMED(SEED_GOLD_MAGNET), NAMED(SEED_SPIKEROCK), NAMED(SEED_COBCANNON),
	NAMED(SEED_IMITATER),
};

#undef NAMED

template <size_t N>
int LookUp(const NamedValue (&theTable)[N], const std::string& theName, const char* theWhat)
{
	for (const NamedValue& aEntry : theTable)
	{
		if (theName == aEntry.mName)
			return aEntry.mValue;
	}
	throw std::runtime_error(std::string("scenario: unknown ") + theWhat + " '" + theName + "'");
}

// The Adventure level whose lawn the scenario plays on: a level with all five rows open.
int LevelForLawn(const std::string& theLawn)
{
	if (theLawn == "day")
		return 4;
	if (theLawn == "night")
		return 14;
	if (theLawn == "pool")
		return 24;
	if (theLawn == "fog")
		return 34;
	if (theLawn == "roof")
		return 44;
	throw std::runtime_error("scenario: unknown lawn '" + theLawn + "'");
}

double NumberOr(const Json& theObject, const char* theKey, double theDefault)
{
	const Json* aValue = theObject.Find(theKey);
	return (aValue != nullptr && aValue->mKind == Json::Number) ? aValue->mNumber : theDefault;
}

std::string StringOr(const Json& theObject, const char* theKey, const char* theDefault)
{
	const Json* aValue = theObject.Find(theKey);
	return (aValue != nullptr && aValue->mKind == Json::String) ? aValue->mString : theDefault;
}

// The anim_* track whose frame range is the one the reanimation is playing, or "" when none is.
std::string CurrentTrackName(Reanimation* theReanim)
{
	if (theReanim == nullptr || theReanim->mDefinition == nullptr)
		return "";
	for (int i = 0; i < theReanim->mDefinition->mTracks.count; i++)
	{
		const char* aName = theReanim->mDefinition->mTracks.tracks[i].mName;
		if (strncmp(aName, "anim_", 5) != 0)
			continue;
		int aStart = 0;
		int aCount = 0;
		theReanim->GetFramesForLayer(aName, aStart, aCount);
		if (aStart == theReanim->mFrameStart && aCount == theReanim->mFrameCount)
			return aName;
	}
	return "";
}

void WriteTick(FILE* theOut, LawnApp* theApp, Board* theBoard, int theTick)
{
	fprintf(theOut, "{\"tick\":%d,\"zombies\":[", theTick);
	bool aFirst = true;
	int anIndex = 0;
	for (Zombie* aZombie : theBoard->mZombies)
	{
		int aZombieIndex = anIndex++;
		if (aZombie->mDead)
			continue;
		Reanimation* aReanim = theApp->ReanimationTryToGet(aZombie->mBodyReanimID);
		fprintf(theOut,
			"%s{\"id\":%d,\"type\":%d,\"row\":%d,\"x\":%.4f,\"y\":%.4f,\"altitude\":%.4f,\"velX\":%.5f,"
			"\"bodyHealth\":%d,\"helmHealth\":%d,\"shieldHealth\":%d,\"phase\":%d,\"height\":%d,"
			"\"eating\":%s,\"dying\":%s,\"track\":\"%s\",\"animTime\":%.5f,\"frameStart\":%d,\"frameCount\":%d}",
			aFirst ? "" : ",", aZombieIndex, static_cast<int>(aZombie->mZombieType), aZombie->mRow,
			aZombie->mPosX, aZombie->mPosY, aZombie->mAltitude, aZombie->mVelX, aZombie->mBodyHealth,
			aZombie->mHelmHealth, aZombie->mShieldHealth, static_cast<int>(aZombie->mZombiePhase),
			static_cast<int>(aZombie->mZombieHeight), aZombie->mIsEating ? "true" : "false",
			aZombie->IsDeadOrDying() ? "true" : "false", CurrentTrackName(aReanim).c_str(),
			aReanim != nullptr ? aReanim->mAnimTime : 0.0f, aReanim != nullptr ? aReanim->mFrameStart : 0,
			aReanim != nullptr ? aReanim->mFrameCount : 0);
		aFirst = false;
	}
	fprintf(theOut, "],\"plants\":[");
	aFirst = true;
	int aPlantIndex = 0;
	for (Plant* aPlant : theBoard->mPlants)
	{
		int aId = aPlantIndex++;
		if (aPlant->mDead)
			continue;
		fprintf(theOut, "%s{\"id\":%d,\"seed\":%d,\"col\":%d,\"row\":%d,\"health\":%d}", aFirst ? "" : ",", aId,
			static_cast<int>(aPlant->mSeedType), aPlant->mPlantCol, aPlant->mRow, aPlant->mPlantHealth);
		aFirst = false;
	}
	fprintf(theOut, "]}\n");
}

} // namespace

int ScenarioRun::Run(const std::string& theScenarioPath, const std::string& theOutPath)
{
	std::ifstream aFile(theScenarioPath);
	if (!aFile)
	{
		fprintf(stderr, "scenario: cannot read %s\n", theScenarioPath.c_str());
		return 2;
	}
	std::stringstream aText;
	aText << aFile.rdbuf();

	FILE* anOut = fopen(theOutPath.c_str(), "w");
	if (anOut == nullptr)
	{
		fprintf(stderr, "scenario: cannot write %s\n", theOutPath.c_str());
		return 2;
	}

	try
	{
		Json aScenario = JsonReader(aText.str()).Read();
		int aLevel = LevelForLawn(StringOr(aScenario, "lawn", "day"));
		ZombieType aZombieType = static_cast<ZombieType>(LookUp(gZombieNames, StringOr(aScenario, "zombie", ""), "zombie"));
		int aRow = static_cast<int>(NumberOr(aScenario, "row", 2));
		int aTicks = static_cast<int>(NumberOr(aScenario, "seconds", 20) * 100.0);

		mApp->mPlayerInfo->SetLevel(aLevel);
		mApp->mGameMode = GameMode::GAMEMODE_ADVENTURE;
		mApp->MakeNewBoard();
		Board* aBoard = mApp->mBoard;
		aBoard->InitLevel();
		mApp->mBoardResult = BoardResult::BOARDRESULT_NONE;
		mApp->mGameScene = GameScenes::SCENE_PLAYING;

		// No waves, mowers or sky sun: the scenario places everything it measures.
		aBoard->mCurrentWave = aBoard->mNumWaves;
		for (LawnMower* aMower : aBoard->mLawnMowers)
		{
			aMower->mDead = true;
		}

		const Json* aPlants = aScenario.Find("plants");
		if (aPlants != nullptr)
		{
			for (const Json& aPlantSpec : aPlants->mItems)
			{
				SeedType aSeed = static_cast<SeedType>(LookUp(gSeedNames, StringOr(aPlantSpec, "seed", ""), "seed"));
				aBoard->NewPlant(static_cast<int>(NumberOr(aPlantSpec, "col", 0)),
					static_cast<int>(NumberOr(aPlantSpec, "row", aRow)), aSeed, SeedType::SEED_NONE);
			}
		}

		Zombie* aZombie = aBoard->AddZombieInRow(aZombieType, aRow, -1);
		if (aZombie == nullptr)
			throw std::runtime_error("scenario: the board refused the zombie");
		const Json* aStart = aScenario.Find("startColumn");
		if (aStart != nullptr && aStart->mKind == Json::Number)
		{
			aZombie->mPosX = static_cast<float>(LAWN_XMIN) + 80.0f * static_cast<float>(aStart->mNumber);
		}

		// Companions keep the port's level alive while the tracked zombie is out of play; they spawn after it.
		const Json* aCompanions = aScenario.Find("companions");
		if (aCompanions != nullptr)
		{
			for (const Json& aCompanion : aCompanions->mItems)
			{
				ZombieType aType = static_cast<ZombieType>(LookUp(gZombieNames, StringOr(aCompanion, "zombie", ""), "zombie"));
				if (aBoard->AddZombieInRow(aType, static_cast<int>(NumberOr(aCompanion, "row", 0)), -1) == nullptr)
					throw std::runtime_error("scenario: the board refused a companion zombie");
			}
		}

		WriteTick(anOut, mApp, aBoard, 0);
		for (int aTick = 1; aTick <= aTicks; aTick++)
		{
			mApp->mAppCounter++;
			aBoard->ProcessDeleteQueue();
			aBoard->Update();
			WriteTick(anOut, mApp, aBoard, aTick);
		}
	}
	catch (const std::exception& anError)
	{
		fprintf(stderr, "%s\n", anError.what());
		fclose(anOut);
		return 2;
	}

	fclose(anOut);
	return 0;
}
