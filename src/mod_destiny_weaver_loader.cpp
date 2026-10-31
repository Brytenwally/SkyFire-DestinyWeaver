/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

// The core's module loader calls Add<folder name>Scripts(); the module lives in
// modules/mod-destiny-weaver.
void AddSC_destiny_weaver();
void AddSC_destiny_weaver_scaling();
void AddSC_destiny_weaver_items();

void Addmod_destiny_weaverScripts()
{
    AddSC_destiny_weaver();
    AddSC_destiny_weaver_scaling();
    AddSC_destiny_weaver_items();
}
