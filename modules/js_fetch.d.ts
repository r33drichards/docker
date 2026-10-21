/** Native asynchronous jobs, not browser Fetch/Promises. Fetch creation requires an active admin callback; polling/cancel require admin authority. */
interface IrcdApi {
  fetch(url: string, options?: {method?: 'GET' | 'POST'; body?: string}): string;
  /** null pending; ready results consumed. body null for invalid UTF-8, bodyBytes exact. */
  takeFetchResult(id: string): {ok: boolean; status: number; body: string | null; bodyBytes: Uint8Array; error: string | null; errorCode: 'dns' | 'connect' | 'timeout' | 'tls' | 'oversize' | 'cancelled' | 'expired' | 'transport' | null} | null;
  /** Best-effort; cannot undo an already transmitted POST. */
  cancelFetch(id: string): boolean;
}
