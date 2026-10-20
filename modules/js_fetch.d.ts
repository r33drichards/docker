/** Native asynchronous jobs, not browser Fetch/Promises. Admin callback only. */
interface IrcdApi {
  fetch(url: string, options?: {method?: 'GET' | 'POST'; body?: string}): string;
  /** null pending; ready results consumed. body null for invalid UTF-8, bodyBytes exact. */
  takeFetchResult(id: string): {ok: boolean; status: number; body: string | null; bodyBytes: Uint8Array; error: string | null} | null;
  /** Best-effort; cannot undo an already transmitted POST. */
  cancelFetch(id: string): boolean;
}
