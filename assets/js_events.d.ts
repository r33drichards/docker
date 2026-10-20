/** Optional-provider event snapshots. Referenced by js_api.d.ts. */
interface HookAwayChange {
  user: HookUser | null; previouslyAway: boolean; previousMessage: HookText;
  previousTime: number; away: boolean; message: HookText; time: number;
}
interface HookWhoRequest {
  source: HookUser | null; matchText: HookText; matchChannel: HookChannel | null;
  fuzzyMatch: boolean; whox: boolean; queryType: HookText;
}
interface HookServerEvent {serverId: HookText; serverName: HookText; serverDescription: HookText}
interface HookHttpEvent {method: HookText; path: HookText; address: HookText}
interface ModuleHookEvents {
  accountChange: {user: HookUser | null; account: HookText; loggedIn: boolean};
  userPreAway: {user: HookUser | null; message: HookText};
  userPreBack: {user: HookUser | null};
  userAway: HookAwayChange;
  userBack: HookAwayChange;
  capAddDel: {name: HookText; adding: boolean};
  capValueChange: {name: HookText};
  namesListItem: {issuer: HookUser | null; membership: HookMembership | null; prefixes: HookText; nick: HookText};
  stats: {source: HookUser | null; symbol: HookText};
  whois: {source: HookUser | null; target: HookUser | null};
  whoisLine: {source: HookUser | null; target: HookUser | null; numeric: HookNumeric};
  whoLine: HookWhoRequest & {user: HookUser | null; membership: HookMembership | null; numeric: HookNumeric};
  whoMatch: HookWhoRequest & {user: HookUser | null};
  whoVisible: HookWhoRequest & {membership: HookMembership | null};
  serverLink: HookServerEvent;
  serverBurst: HookServerEvent;
  serverSplit: HookServerEvent & {error: boolean};
  syncUser: HookServerEvent & {user: HookUser | null};
  syncChannel: HookServerEvent & {channel: HookChannel | null};
  syncNetwork: HookServerEvent;
  httpACLCheck: HookHttpEvent;
  httpRequest: HookHttpEvent;
}
