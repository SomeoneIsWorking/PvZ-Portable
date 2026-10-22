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
// spawn point. Optional board seeds: "mode" (a GAMEMODE_* name, the board's mode in place of the
// lawn's Adventure level), "sun" (mSunMoney) and "money" (the player's mCoins). A plant with
// "at": seconds is planted then through Board::AddPlant, as a player plants, instead of with the
// zombie. "frames": [seconds] draws the screen at each and writes oracle-frame-<tick>.png beside the
// trace. "zombie" may be null for a board with no tracked zombie. "beaten": true records the mode's
// challenge as won before the board starts, so the board plays as a replay. "mowers": true keeps
// the board's mowers, which are otherwise taken off. "presses": [ { "at": seconds, "packet":
// index } ] clicks that seed packet as the player does. A line is { "tick": n, "appCounter": n
// (what the dancers dance to), "zombies": [...], "plants": [...], "coins": [...], "sun": n,
// "money": n }; tick 0 is before any update.

#include "ScenarioRun.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../LawnApp.h"
#include "../../GameConstants.h"
#include "../../ConstEnums.h"
#include "../../PvzpLib/Reanimator.h"
#include "../Board.h"
#include "../Challenge.h"
#include "../Coin.h"
#include "../MessageWidget.h"
#include "../Plant.h"
#include "../SeedPacket.h"
#include "../Zombie.h"
#include "../LawnMower.h"
#include "PlayerInfo.h"
#include "graphics/GLPlatform.h"
#include "graphics/GLImage.h"
#include "graphics/GLInterface.h"
#include "imagelib/ImageLib.h"
#include "graphics/Graphics.h"

using namespace Sexy;

namespace
{

struct Json
{
	enum Kind { Null, Bool, Number, String, Array, Object } mKind = Null;
	bool mBool = false;
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
		for (bool aBool : { true, false })
		{
			const char* aWord = aBool ? "true" : "false";
			size_t aLength = std::strlen(aWord);
			if (mText.compare(mPos, aLength, aWord) == 0)
			{
				mPos += aLength;
				aValue.mKind = Json::Bool;
				aValue.mBool = aBool;
				return aValue;
			}
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
	NAMED(SEED_IMITATER), NAMED(SEED_EXPLODE_O_NUT), NAMED(SEED_GIANT_WALLNUT),
};

const NamedValue gGameModeNames[] = {
	NAMED(GAMEMODE_ADVENTURE), NAMED(GAMEMODE_CHALLENGE_WAR_AND_PEAS), NAMED(GAMEMODE_CHALLENGE_WALLNUT_BOWLING),
	NAMED(GAMEMODE_CHALLENGE_SLOT_MACHINE), NAMED(GAMEMODE_CHALLENGE_RAINING_SEEDS), NAMED(GAMEMODE_CHALLENGE_BEGHOULED),
	NAMED(GAMEMODE_CHALLENGE_INVISIGHOUL), NAMED(GAMEMODE_CHALLENGE_SEEING_STARS), NAMED(GAMEMODE_CHALLENGE_ZOMBIQUARIUM),
	NAMED(GAMEMODE_CHALLENGE_BEGHOULED_TWIST), NAMED(GAMEMODE_CHALLENGE_LITTLE_TROUBLE),
	NAMED(GAMEMODE_CHALLENGE_PORTAL_COMBAT), NAMED(GAMEMODE_CHALLENGE_COLUMN), NAMED(GAMEMODE_CHALLENGE_BOBSLED_BONANZA),
	NAMED(GAMEMODE_CHALLENGE_SPEED), NAMED(GAMEMODE_CHALLENGE_WHACK_A_ZOMBIE), NAMED(GAMEMODE_CHALLENGE_LAST_STAND),
	NAMED(GAMEMODE_CHALLENGE_WAR_AND_PEAS_2), NAMED(GAMEMODE_CHALLENGE_WALLNUT_BOWLING_2),
	NAMED(GAMEMODE_CHALLENGE_POGO_PARTY), NAMED(GAMEMODE_CHALLENGE_FINAL_BOSS),
	NAMED(GAMEMODE_CHALLENGE_ART_CHALLENGE_WALLNUT), NAMED(GAMEMODE_CHALLENGE_SUNNY_DAY),
	NAMED(GAMEMODE_CHALLENGE_RESODDED), NAMED(GAMEMODE_CHALLENGE_BIG_TIME),
	NAMED(GAMEMODE_CHALLENGE_ART_CHALLENGE_SUNFLOWER), NAMED(GAMEMODE_CHALLENGE_AIR_RAID), NAMED(GAMEMODE_CHALLENGE_ICE),
	NAMED(GAMEMODE_CHALLENGE_HIGH_GRAVITY), NAMED(GAMEMODE_CHALLENGE_GRAVE_DANGER),
	NAMED(GAMEMODE_CHALLENGE_SHOVEL), NAMED(GAMEMODE_CHALLENGE_STORMY_NIGHT), NAMED(GAMEMODE_CHALLENGE_BUNGEE_BLITZ),
	NAMED(GAMEMODE_CHALLENGE_SQUIRREL), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_1), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_2),
	NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_3), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_4), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_5),
	NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_6), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_7), NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_8),
	NAMED(GAMEMODE_PUZZLE_I_ZOMBIE_9),
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
	if (theLawn == "roof-night")
		return 50;
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
	fprintf(theOut, "{\"tick\":%d,\"appCounter\":%d,\"zombies\":[", theTick, theApp->mAppCounter);
	bool aFirst = true;
	for (Zombie* aZombie : theBoard->mZombies)
	{
		unsigned int aZombieIndex = theBoard->mZombies.DataArrayGetID(aZombie);
		if (aZombie->mDead)
			continue;
		Reanimation* aReanim = theApp->ReanimationTryToGet(aZombie->mBodyReanimID);
		fprintf(theOut,
			"%s{\"id\":%u,\"type\":%d,\"row\":%d,\"x\":%.4f,\"y\":%.4f,\"altitude\":%.4f,\"velX\":%.5f,"
			"\"bodyHealth\":%d,\"helmHealth\":%d,\"shieldHealth\":%d,\"phase\":%d,\"height\":%d,"
			"\"eating\":%s,\"dying\":%s,\"track\":\"%s\",\"animTime\":%.5f,\"frameStart\":%d,\"frameCount\":%d,"
			"\"related\":%u,\"phaseCounter\":%d}",
			aFirst ? "" : ",", aZombieIndex, static_cast<int>(aZombie->mZombieType), aZombie->mRow,
			aZombie->mPosX, aZombie->mPosY, aZombie->mAltitude, aZombie->mVelX, aZombie->mBodyHealth,
			aZombie->mHelmHealth, aZombie->mShieldHealth, static_cast<int>(aZombie->mZombiePhase),
			static_cast<int>(aZombie->mZombieHeight), aZombie->mIsEating ? "true" : "false",
			aZombie->IsDeadOrDying() ? "true" : "false", CurrentTrackName(aReanim).c_str(),
			aReanim != nullptr ? aReanim->mAnimTime : 0.0f, aReanim != nullptr ? aReanim->mFrameStart : 0,
			aReanim != nullptr ? aReanim->mFrameCount : 0, static_cast<unsigned int>(aZombie->mRelatedZombieID),
			aZombie->mPhaseCounter);
		aFirst = false;
	}
	fprintf(theOut, "],\"plants\":[");
	aFirst = true;
	for (Plant* aPlant : theBoard->mPlants)
	{
		// The data array's id, which a later object in the same slot does not share.
		unsigned int aId = theBoard->mPlants.DataArrayGetID(aPlant);
		if (aPlant->mDead)
			continue;
		Reanimation* aReanim = theApp->ReanimationTryToGet(aPlant->mBodyReanimID);
		fprintf(theOut,
			"%s{\"id\":%u,\"seed\":%d,\"col\":%d,\"row\":%d,\"health\":%d,\"x\":%d,\"y\":%d,\"rowY\":%d,"
			"\"state\":%d,\"launches\":%d,\"animRate\":%.4f}",
			aFirst ? "" : ",", aId, static_cast<int>(aPlant->mSeedType), aPlant->mPlantCol, aPlant->mRow,
			aPlant->mPlantHealth, aPlant->mX, aPlant->mY, theBoard->GridToPixelY(0, aPlant->mRow),
			static_cast<int>(aPlant->mState), aPlant->mLaunchCounter, aReanim != nullptr ? aReanim->mAnimRate : 0.0f);
		aFirst = false;
	}
	fprintf(theOut, "],\"coins\":[");
	aFirst = true;
	for (Coin* aCoin : theBoard->mCoins)
	{
		if (aCoin->mDead)
			continue;
		fprintf(theOut, "%s{\"id\":%u,\"type\":%d,\"x\":%.2f,\"y\":%.2f,\"collected\":%s}", aFirst ? "" : ",",
			theBoard->mCoins.DataArrayGetID(aCoin), static_cast<int>(aCoin->mType), aCoin->mPosX, aCoin->mPosY,
			aCoin->mIsBeingCollected ? "true" : "false");
		aFirst = false;
	}
	fprintf(theOut, "],\"belt\":[");
	aFirst = true;
	for (int i = 0; theBoard->HasConveyorBeltSeedBank() && i < theBoard->mSeedBank->mNumPackets; i++)
	{
		// Where the packet is drawn on the bank: its place plus how far it still has to ride.
		const SeedPacket& aPacket = theBoard->mSeedBank->mSeedPackets[i];
		if (aPacket.mPacketType == SeedType::SEED_NONE)
			continue;
		fprintf(theOut, "%s{\"seed\":%d,\"x\":%d}", aFirst ? "" : ",", static_cast<int>(aPacket.mPacketType),
			aPacket.mX + aPacket.mOffsetX);
		aFirst = false;
	}
	fprintf(theOut, "],\"advice\":%d,\"adviceShown\":%s,\"tutorial\":%d,\"sun\":%d,\"money\":%d}\n",
		static_cast<int>(theBoard->mHelpIndex), theBoard->mAdvice->IsBeingDisplayed() ? "true" : "false",
		static_cast<int>(theBoard->mTutorialState), theBoard->mSunMoney, theApp->mPlayerInfo->mCoins);
}

// Draws the board as its widget draws itself and writes the presented pixels as a PNG; the title
// screen the scenario was started from is still the top widget, so the screen is not drawn whole.
void WriteFrame(LawnApp* theApp, Board* theBoard, const std::string& thePath)
{
	theApp->mGLInterface->PreDraw();
	Graphics aGraphics(theApp->mGLInterface->GetScreenImage());
	theBoard->Draw(&aGraphics);
	const Rect& aRect = theApp->mGLInterface->mPresentationRect;
	std::vector<uint32_t> aPixels(static_cast<size_t>(aRect.mWidth) * aRect.mHeight);
	glReadPixels(aRect.mX, aRect.mY, aRect.mWidth, aRect.mHeight, GL_RGBA, GL_UNSIGNED_BYTE, aPixels.data());
	ImageLib::Image anImage;
	anImage.mWidth = aRect.mWidth;
	anImage.mHeight = aRect.mHeight;
	anImage.mBits = std::make_unique<uint32_t[]>(aPixels.size());
	// GL rows run bottom up and its bytes are RGBA; the image is top down ARGB.
	for (int aRow = 0; aRow < aRect.mHeight; aRow++)
	{
		const uint32_t* aSrc = aPixels.data() + static_cast<size_t>(aRect.mHeight - 1 - aRow) * aRect.mWidth;
		uint32_t* aDst = anImage.mBits.get() + static_cast<size_t>(aRow) * aRect.mWidth;
		for (int aCol = 0; aCol < aRect.mWidth; aCol++)
		{
			uint32_t aRgba = aSrc[aCol];
			aDst[aCol] = 0xFF000000u | ((aRgba & 0xFFu) << 16) | (aRgba & 0xFF00u) | ((aRgba >> 16) & 0xFFu);
		}
	}
	if (!ImageLib::WritePNGImage(thePath, &anImage))
		throw std::runtime_error("scenario: cannot write " + thePath);
	theApp->mGLInterface->Redraw(nullptr);
}

struct TimedPress
{
	int mTick;
	int mPacket;
};

struct TimedPlant
{
	int mTick;
	int mCol;
	int mRow;
	SeedType mSeed;
};

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
		const Json* aZombieName = aScenario.Find("zombie");
		bool aHasZombie = aZombieName != nullptr && aZombieName->mKind == Json::String;
		ZombieType aZombieType = aHasZombie ? static_cast<ZombieType>(LookUp(gZombieNames, aZombieName->mString, "zombie"))
		                                    : ZombieType::ZOMBIE_INVALID;
		int aRow = static_cast<int>(NumberOr(aScenario, "row", 2));
		int aTicks = static_cast<int>(NumberOr(aScenario, "seconds", 20) * 100.0);

		mApp->mPlayerInfo->SetLevel(aLevel);
		const Json* aMode = aScenario.Find("mode");
		mApp->mGameMode = aMode != nullptr ? static_cast<GameMode>(LookUp(gGameModeNames, aMode->mString, "mode"))
		                                    : GameMode::GAMEMODE_ADVENTURE;
		const Json* aBeaten = aScenario.Find("beaten");
		if (aBeaten != nullptr && aBeaten->mKind == Json::Bool && aBeaten->mBool)
			mApp->mPlayerInfo->mChallengeRecords[mApp->GetCurrentChallengeIndex()] = 1;
		mApp->MakeNewBoard();
		Board* aBoard = mApp->mBoard;
		aBoard->InitLevel();
		mApp->mBoardResult = BoardResult::BOARDRESULT_NONE;
		mApp->mGameScene = GameScenes::SCENE_PLAYING;
		// The mode's own start: Zombiquarium's two snorkels, the belt's first delay.
		aBoard->mChallenge->StartLevel();

		// No waves or sky sun, and no mowers unless asked: the scenario places everything it measures.
		aBoard->mCurrentWave = aBoard->mNumWaves;
		// An Adventure board's belt deals only its opening packets: 5-10's picks read a boss the scenario does not place.
		if (aMode == nullptr && aBoard->HasConveyorBeltSeedBank())
			aBoard->mChallenge->mConveyorBeltCounter = INT_MAX;
		// CutScene::PlaceLawnItems lays the mowers, which a scenario skips; a roof's need the Roof Cleaner.
		const Json* aMowers = aScenario.Find("mowers");
		if (aMowers != nullptr && aMowers->mKind == Json::Bool && aMowers->mBool)
		{
			mApp->mPlayerInfo->mPurchases[StoreItem::STORE_ITEM_ROOF_CLEANER] = 1;
			aBoard->InitLawnMowers();
			for (LawnMower* aMower : aBoard->mLawnMowers)
			{
				// Where CutScene::Update's roll-in leaves them.
				aMower->mVisible = true;
				aMower->mPosX = -21.0f;
			}
		}

		aBoard->mSunMoney = static_cast<int>(NumberOr(aScenario, "sun", aBoard->mSunMoney));
		mApp->mPlayerInfo->mCoins = static_cast<int>(NumberOr(aScenario, "money", mApp->mPlayerInfo->mCoins));

		std::vector<TimedPlant> aTimedPlants;
		const Json* aPlants = aScenario.Find("plants");
		if (aPlants != nullptr)
		{
			for (const Json& aPlantSpec : aPlants->mItems)
			{
				SeedType aSeed = static_cast<SeedType>(LookUp(gSeedNames, StringOr(aPlantSpec, "seed", ""), "seed"));
				int aCol = static_cast<int>(NumberOr(aPlantSpec, "col", 0));
				int aPlantRow = static_cast<int>(NumberOr(aPlantSpec, "row", aRow));
				const Json* anAt = aPlantSpec.Find("at");
				if (anAt != nullptr && anAt->mKind == Json::Number)
					aTimedPlants.push_back({ static_cast<int>(anAt->mNumber * 100.0 + 0.5), aCol, aPlantRow, aSeed });
				else
					aBoard->NewPlant(aCol, aPlantRow, aSeed, SeedType::SEED_NONE);
			}
		}

		std::vector<TimedPress> aPresses;
		const Json* aPressSpecs = aScenario.Find("presses");
		if (aPressSpecs != nullptr)
		{
			for (const Json& aPress : aPressSpecs->mItems)
			{
				int aPacket = static_cast<int>(NumberOr(aPress, "packet", -1));
				if (aPacket < 0 || aPacket >= aBoard->mSeedBank->mNumPackets)
					throw std::runtime_error("scenario: a press names no seed packet");
				aPresses.push_back({ static_cast<int>(NumberOr(aPress, "at", 0) * 100.0 + 0.5), aPacket });
			}
		}

		std::vector<int> aFrameTicks;
		const Json* aFrames = aScenario.Find("frames");
		if (aFrames != nullptr)
		{
			for (const Json& aFrame : aFrames->mItems)
				aFrameTicks.push_back(static_cast<int>(aFrame.mNumber * 100.0 + 0.5));
		}
		size_t aSlash = theOutPath.find_last_of('/');
		std::string aFrameDir = aSlash == std::string::npos ? std::string(".") : theOutPath.substr(0, aSlash);

		Zombie* aZombie = aHasZombie ? aBoard->AddZombieInRow(aZombieType, aRow, -1) : nullptr;
		if (aHasZombie && aZombie == nullptr)
			throw std::runtime_error("scenario: the board refused the zombie");
		const Json* aStart = aScenario.Find("startColumn");
		if (aZombie != nullptr && aStart != nullptr && aStart->mKind == Json::Number)
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
			for (const TimedPlant& aPlant : aTimedPlants)
			{
				if (aPlant.mTick == aTick)
					aBoard->AddPlant(aPlant.mCol, aPlant.mRow, aPlant.mSeed, SeedType::SEED_NONE);
			}
			for (const TimedPress& aPress : aPresses)
			{
				if (aPress.mTick == aTick)
					aBoard->mSeedBank->mSeedPackets[aPress.mPacket].MouseDown(0, 0, 1);
			}
			mApp->mAppCounter++;
			aBoard->ProcessDeleteQueue();
			aBoard->Update();
			WriteTick(anOut, mApp, aBoard, aTick);
			for (int aFrameTick : aFrameTicks)
			{
				if (aFrameTick == aTick)
					WriteFrame(mApp, aBoard, aFrameDir + "/oracle-frame-" + std::to_string(aTick) + ".png");
			}
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
