# Module hook coverage (InspIRCd 4.12.1)

All 74 Implementation events plus the direct OnCleanup callback. Native references are owned read-only snapshots; only command rewrites and explicit queued effects mutate native state.

| Native hook | JS stage | Result | Callback failure |
|---|---|---|---|
| OnUserConnect | `userConnect` | observe | observe only |
| OnUserPreQuit | `userPreQuit` | pass/deny/allow (admin) | pass |
| OnUserQuit | `userQuit` | observe | observe only |
| OnUserDisconnect | `userDisconnect` | observe | observe only |
| OnChannelPreDelete | `channelPreDelete` | pass/deny/allow (admin) | pass |
| OnChannelDelete | `channelDelete` | observe | observe only |
| OnUserJoin | `userJoin` | observe | observe only |
| OnPostJoin | `postJoin` | observe | observe only |
| OnUserPart | `userPart` | observe | observe only |
| OnPreRehash | `preRehash` | observe | observe only |
| OnModuleRehash | `moduleRehash` | observe | observe only |
| OnSendSnotice | `sendSnotice` | pass/deny/allow (admin) | deny |
| OnUserPreJoin | `userPreJoin` | pass/deny/allow (admin) | deny |
| OnUserPreKick | `userPreKick` | pass/deny/allow (admin) | deny |
| OnUserKick | `userKick` | observe | observe only |
| OnUserPreInvite | `userPreInvite` | pass/deny/allow (admin) | deny |
| OnUserInvite | `userInvite` | observe | observe only |
| OnUserPreMessage | `userPreMessage` | pass/deny/allow (admin) | deny |
| OnBuildNeighborList | `buildNeighborList` | observe | observe only |
| OnUserPreNick | `userPreNick` | pass/deny/allow (admin) | deny |
| OnUserPostMessage | `userPostMessage` | observe | observe only |
| OnUserMessage | `userMessage` | observe | observe only |
| OnUserMessageBlocked | `userMessageBlocked` | observe | observe only |
| OnMode | `mode` | observe | observe only |
| OnDecodeMetadata | `decodeMetadata` | observe | observe only |
| OnChangeHost | `changeHost` | observe | observe only |
| OnChangeRealHost | `changeRealHost` | observe | observe only |
| OnPostChangeRealHost | `postChangeRealHost` | observe | observe only |
| OnChangeRealName | `changeRealName` | observe | observe only |
| OnChangeUser | `changeUser` | observe | observe only |
| OnChangeRealUser | `changeRealUser` | observe | observe only |
| OnPostChangeRealUser | `postChangeRealUser` | observe | observe only |
| OnAddLine | `addLine` | observe | observe only |
| OnDelLine | `delLine` | observe | observe only |
| OnExpireLine | `expireLine` | observe | observe only |
| OnCleanup | `cleanup` | observe | observe only |
| OnUserPostNick | `userPostNick` | observe | observe only |
| OnPreMode | `preMode` | pass/deny/allow (admin) | deny |
| OnKill | `kill` | pass/deny/allow (admin) | pass |
| OnLoadModule | `loadModule` | observe | observe only |
| OnUnloadModule | `unloadModule` | observe | observe only |
| OnBackgroundTimer | `backgroundTimer` | observe | observe only |
| OnPreCommand | `preCommand` | pass/deny | deny |
| OnPostCommand | `postCommand` | observe | observe only |
| OnCommandBlocked | `commandBlocked` | observe | observe only |
| OnUserInit | `userInit` | observe | observe only |
| OnUserPostInit | `userPostInit` | observe | observe only |
| OnCheckReady | `checkReady` | pass/deny/allow (admin) | deny |
| OnUserRegister | `userRegister` | pass/deny/allow (admin) | deny |
| OnCheckInvite | `checkInvite` | pass/deny/allow (admin) | deny |
| OnRawMode | `rawMode` | pass/deny/allow (admin) | deny |
| OnCheckKey | `checkKey` | pass/deny/allow (admin) | deny |
| OnCheckLimit | `checkLimit` | pass/deny/allow (admin) | deny |
| OnCheckChannelBan | `checkChannelBan` | pass/deny/allow (admin) | deny |
| OnCheckBan | `checkBan` | pass/deny/allow (admin) | deny |
| OnCheckPassword | `checkPassword` | pass/deny/allow (admin) | deny |
| OnPreTopicChange | `preTopicChange` | pass/deny/allow (admin) | deny |
| OnPostTopicChange | `postTopicChange` | observe | observe only |
| OnPostConnect | `postConnect` | observe | observe only |
| OnAcceptConnection | `acceptConnection` | pass/deny | deny |
| OnGarbageCollect | `garbageCollect` | observe | observe only |
| OnNumeric | `numeric` | pass/deny/allow (admin) | deny |
| OnChangeRemoteAddress | `changeRemoteAddress` | observe | observe only |
| OnServiceAdd | `serviceAdd` | observe | observe only |
| OnServiceDel | `serviceDel` | observe | observe only |
| OnUserWrite | `userWrite` | pass/deny/allow (admin) | deny |
| OnShutdown | `shutdown` | observe | observe only |
| OnPreOperLogin | `preOperLogin` | pass/deny/allow (admin) | deny |
| OnOperLogin | `operLogin` | observe | observe only |
| OnPostOperLogin | `postOperLogin` | observe | observe only |
| OnOperLogout | `operLogout` | observe | observe only |
| OnPostOperLogout | `postOperLogout` | observe | observe only |
| OnPreChangeConnectClass | `preChangeConnectClass` | pass/deny/allow (admin) | deny |
| OnChangeConnectClass | `changeConnectClass` | observe | observe only |
| OnPostChangeConnectClass | `postChangeConnectClass` | observe | observe only |
