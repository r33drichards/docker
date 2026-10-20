/// <reference path="js_generated/module_hooks.d.ts" />
/** Prefix copies: bytes are exact only when truncated=false. Invalid UTF-8 -> null. */
interface HookText {text: string | null; bytes: Uint8Array; byteLength: number; truncated: boolean}
interface HookList<T> {items: T[]; length: number; truncated: boolean}
interface HookUser {id: string; nick: HookText; local: boolean; quitting: boolean; address: HookText}
interface HookChannel {name: HookText}
interface HookMembership {user: HookUser | null; channel: HookChannel | null; prefixes: HookText}
interface HookExtensible {kind: HookText; value: HookUser | HookChannel | HookMembership}
interface HookTarget {
  type: number; name: HookText; status: HookText; originalStatus: HookText;
  user: HookUser | null; channel: HookChannel | null;
}
interface HookMessageDetails {
  type: number; text: HookText; originalText: HookText; echo: boolean; echoOriginal: boolean;
  updateIdle: boolean; exemptions: HookList<HookUser | null>;
  tagsIn: HookList<{name: HookText; value: HookText}>; tagsOut: HookList<{name: HookText; value: HookText}>;
}
interface HookMode {adding: boolean; mode: HookText; name: HookText; parameter: HookText}
interface HookXLine {
  type: HookText; mask: HookText; source: HookText; reason: HookText;
  setTime: number; duration: number; expiry: number; fromConfig: boolean;
}
interface HookModule {file: HookText; description: HookText; version: HookText; dying: boolean}
interface HookAddress {address: HookText; port: number; family: number}
interface HookListener {address: HookAddress; protocol: number}
interface HookNumeric {code: number; params: HookList<HookText>}
interface HookService {name: HookText; type: number}
interface HookWireMessage {
  command: HookText; params: HookList<HookText>; source: HookText | null;
  tags: HookList<{name: HookText; value: HookText}>;
}
type HookDecision = {action: 'pass' | 'deny' | 'allow'};
type ModuleHookResult<K extends keyof ModuleHookEvents> = HookDecision |
  (K extends keyof ModuleHookPatches ? {action: 'pass'; patch: ModuleHookPatches[K]} : never);
type ModuleHookEvent<K extends keyof ModuleHookEvents> = ModuleHookEvents[K] & {stage: K; userId: string | null};
interface IrcdApi {
  /** Native result semantics apply. allow requires authority=admin, never accepts socket ownership.
   * Explicit patches are available only on ModuleHookPatches stages. Void patches require action=pass.
   * Patches commit atomically on successful dispatch; snapshots remain detached copies, not native handles.
   */
  on<K extends keyof ModuleHookEvents>(stage: K, callback: (event: ModuleHookEvent<K>) => ModuleHookResult<K> | void): void;
}
