/// <reference path="js_fetch.d.ts" />
/// <reference path="js_events.d.ts" />
/// <reference path="js_hooks.d.ts" />
/// <reference path="js_generated/inspircd.d.ts" />
/** Owned snapshots. Text is null for invalid UTF-8; raw byte arrays are exact. */
interface CommandEvent {
  command: string | null;
  commandBytes: Uint8Array;
  params: (string | null)[];
  paramBytes: Uint8Array[];
  userId: string;
  stage: 'preCommand' | 'postCommand';
  validated: boolean;
  result: 'SUCCESS' | 'FAILURE' | 'INVALID' | null;
  loop: boolean;
}
type CommandDecision = { action: 'deny' } | {
  action: 'pass'; command?: string; params?: (string | Uint8Array)[];
};
// Additional methods on the global ircd object; authority comes only from config.
interface IrcdApi {
  on(stage: 'preCommand', callback: (event: CommandEvent) => CommandDecision | void): void;
  on(stage: 'postCommand', callback: (event: CommandEvent) => void): void;
  /** Admin callback only. JSON object <=16KiB. true is staged, not delivered; false is queue-full. */
  postWebhook(name: string, json: string): boolean;
  /** Aggregate process/module-lifetime counters; never URLs, headers or payloads. */
  webhookStats(): {pending: number; sent: number; failed: number; discarded: number};
  wasm(name: string): Uint8Array;
  userById(id: string): { id: string; nick: string | null } | null;
  notice(targetId: string, text: string): void;
  rename(targetId: string, nick: string): void;
}

/** Native host methods. Mutations are staged, not delivered; admin authority is configured by the server. */
interface NativeUserSnapshot {
  id: string; nick: string | null; username: string | null; hostname: string | null;
  realname: string | null; local: boolean; oper: boolean;
}
interface NativeChannelSnapshot {
  name: string | null; topic: string | null; modes: string | null; created: number;
  members: (NativeUserSnapshot & {rank: number; prefix: string | null})[]; truncated: boolean;
}
interface IrcdApi {
  userByNick(nick: string): NativeUserSnapshot | null;
  channelByName(name: string): NativeChannelSnapshot | null;
  channels(): string[];
  /** Initialization only, admin scripts only. Refuses native/other-script command conflicts. */
  registerCommand(name: string, minParams: number, maxParams: number, operOnly: boolean,
    callback: (event: {userId: string; params: string[]}) => void): void;
  /** Seconds, 1..86400. At most 32 timers per script; no catch-up bursts. */
  setTimer(seconds: number, repeat: boolean, callback: (event: {userId: null; params: []}) => void): number;
  clearTimer(id: number): boolean;
  join(userId: string, channel: string, key: string): boolean;
  part(userId: string, channel: string, reason: string): boolean;
  kick(userId: string, channel: string, reason: string): boolean;
  setTopic(channel: string, topic: string): boolean;
  setModes(channel: string, modesAndParams: string): boolean;
  setHost(userId: string, hostname: string): boolean;
  setUsername(userId: string, username: string): boolean;
  setRealname(userId: string, realname: string): boolean;
  disconnect(userId: string, reason: string): boolean;
  /** Duration is a decimal seconds string, 0 for permanent. */
  addBan(type: string, mask: string, duration: string, reason: string): boolean;
  removeBan(type: string, mask: string): boolean;
  loadModule(name: string): boolean;
  unloadModule(name: string): boolean;
  rehash(): boolean;
  shutdown(): boolean;
}
