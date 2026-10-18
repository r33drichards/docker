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
  wasm(name: string): Uint8Array;
  userById(id: string): { id: string; nick: string | null } | null;
  notice(targetId: string, text: string): void;
  rename(targetId: string, nick: string): void;
}
