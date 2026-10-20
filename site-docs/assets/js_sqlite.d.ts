/** Additional declaration merging for the explicit SQLite API. */
type SqliteValue = null | number | bigint | string | Uint8Array;
interface SqliteResult {
  columns: string[];
  rows: SqliteValue[][];
  changes: bigint;
  lastInsertRowid: bigint;
}
interface SqliteDatabase {
  query(sql: string, params?: SqliteValue[]): SqliteResult;
  exec(sql: string, params?: SqliteValue[]): SqliteResult;
  transaction<T>(callback: () => T): T;
  close(): void;
}
interface IrcdApi {
  readonly sqlite: {open(name: string): SqliteDatabase};
}
