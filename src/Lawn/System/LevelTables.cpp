/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#include "LevelTables.h"
#include "EnumNames.h"

#include <cstdio>
#include <string>

#include "../../LawnApp.h"
#include "../../ConstEnums.h"
#include "../Board.h"
#include "../SeedPacket.h"
#include "../Zombie.h"
#include "../Widget/ChallengeScreen.h"
#include "PlayerInfo.h"

namespace
{

std::string Quoted(const char* theText)
{
	return theText == nullptr ? std::string("null") : "\"" + std::string(theText) + "\"";
}

// The board InitLevel left, as one JSON object's fields after theId's.
void WriteBoard(FILE* theOut, Board* theBoard)
{
	fprintf(theOut, ",\"level\":%d,\"background\":%s,\"sun\":%d", theBoard->mLevel,
	        Quoted(EnumNames::BackgroundName(theBoard->mBackground)).c_str(), theBoard->mSunMoney);
	fprintf(theOut, ",\"rows\":[");
	for (int aRow = 0; aRow < MAX_GRID_SIZE_Y; aRow++)
		fprintf(theOut, "%s%s", aRow ? "," : "", Quoted(EnumNames::PlantRowName(theBoard->mPlantRow[aRow])).c_str());
	fprintf(theOut, "],\"fog\":%s", theBoard->StageHasFog() ? "true" : "false");

	fprintf(theOut, ",\"allowed\":[");
	bool aFirst = true;
	for (int aType = 0; aType < NUM_ZOMBIE_TYPES; aType++)
	{
		if (!theBoard->mZombieAllowed[aType])
			continue;
		fprintf(theOut, "%s%s", aFirst ? "" : ",", Quoted(EnumNames::ZombieName(aType)).c_str());
		aFirst = false;
	}
	fprintf(theOut, "],\"numWaves\":%d,\"waves\":[", theBoard->mNumWaves);
	for (int aWave = 0; aWave < theBoard->mNumWaves && aWave < MAX_ZOMBIE_WAVES; aWave++)
	{
		int aPoints = 0;
		fprintf(theOut, "%s{\"flag\":%s,\"zombies\":[", aWave ? "," : "", theBoard->IsFlagWave(aWave) ? "true" : "false");
		for (int i = 0; i < MAX_ZOMBIES_IN_WAVE; i++)
		{
			ZombieType aType = theBoard->mZombiesInWave[aWave][i];
			if (aType == ZombieType::ZOMBIE_INVALID)
				break;
			aPoints += GetZombieDefinition(aType).mZombieValue;
			fprintf(theOut, "%s%s", i ? "," : "", Quoted(EnumNames::ZombieName(aType)).c_str());
		}
		fprintf(theOut, "],\"points\":%d}", aPoints);
	}
	fprintf(theOut, "]");

	SeedBank* aBank = theBoard->mSeedBank.get();
	fprintf(theOut, ",\"chooser\":%s,\"conveyor\":%s,\"packets\":%d,\"preset\":[",
	        theBoard->ChooseSeedsOnCurrentLevel() ? "true" : "false", theBoard->HasConveyorBeltSeedBank() ? "true" : "false",
	        aBank->mNumPackets);
	aFirst = true;
	for (int i = 0; i < aBank->mNumPackets; i++)
	{
		SeedType aSeed = aBank->mSeedPackets[i].mPacketType;
		if (aSeed == SeedType::SEED_NONE)
			continue;
		fprintf(theOut, "%s%s", aFirst ? "" : ",", Quoted(EnumNames::SeedName(aSeed)).c_str());
		aFirst = false;
	}
	fprintf(theOut, "]");
}

}

int LevelTables::Write(const std::string& theOutPath)
{
	FILE* anOut = fopen(theOutPath.c_str(), "w");
	if (anOut == nullptr)
	{
		fprintf(stderr, "level-tables: cannot write %s\n", theOutPath.c_str());
		return 2;
	}
	fprintf(anOut, "{\"adventure\":[");
	mApp->mPlayerInfo->mFinishedAdventure = 0;
	for (int aLevel = 1; aLevel <= ADVENTURE_AREAS * LEVELS_PER_AREA; aLevel++)
	{
		mApp->mPlayerInfo->SetLevel(aLevel);
		mApp->mGameMode = GameMode::GAMEMODE_ADVENTURE;
		mApp->MakeNewBoard();
		mApp->mBoard->InitLevel();
		CoinType aCoin = mApp->AdventureAwardCoin(aLevel);
		fprintf(anOut, "%s\n{\"id\":\"%d-%d\",\"award\":%s,\"awardSeed\":%s", aLevel > 1 ? "," : "",
		        (aLevel - 1) / LEVELS_PER_AREA + 1, (aLevel - 1) % LEVELS_PER_AREA + 1, Quoted(EnumNames::CoinName(aCoin)).c_str(),
		        aCoin == CoinType::COIN_FINAL_SEED_PACKET ? Quoted(EnumNames::SeedName(LawnApp::GetAwardSeedForLevel(aLevel))).c_str()
		                                                  : "null");
		WriteBoard(anOut, mApp->mBoard);
		fprintf(anOut, "}");
	}
	fprintf(anOut, "],\n\"challenges\":[");
	// A finished Adventure, which is what opens the challenge pages.
	mApp->mPlayerInfo->mFinishedAdventure = 1;
	mApp->mPlayerInfo->SetLevel(1);
	bool aFirst = true;
	for (const ChallengeDefinition& aDef : gChallengeDefs)
	{
		// Not levels: the garden, the tree, the trial's upsell and the first-run intro.
		if (aDef.mChallengeMode == GameMode::GAMEMODE_CHALLENGE_ZEN_GARDEN ||
		    aDef.mChallengeMode == GameMode::GAMEMODE_TREE_OF_WISDOM || aDef.mChallengeMode == GameMode::GAMEMODE_UPSELL ||
		    aDef.mChallengeMode == GameMode::GAMEMODE_INTRO)
			continue;
		mApp->mGameMode = aDef.mChallengeMode;
		mApp->MakeNewBoard();
		mApp->mBoard->InitLevel();
		std::string aName = aDef.mChallengeName;
		if (aName.size() > 2 && aName.front() == '[' && aName.back() == ']')
			aName = aName.substr(1, aName.size() - 2);
		fprintf(anOut, "%s\n{\"id\":\"%s\",\"page\":%d,\"row\":%d,\"column\":%d", aFirst ? "" : ",", aName.c_str(),
		        static_cast<int>(aDef.mPage), aDef.mRow, aDef.mCol);
		WriteBoard(anOut, mApp->mBoard);
		fprintf(anOut, "}");
		aFirst = false;
	}
	fprintf(anOut, "]}\n");
	fclose(anOut);
	mApp->KillBoard();
	return 0;
}
