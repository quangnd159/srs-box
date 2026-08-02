import { permanentRedirect } from "next/navigation";

// /sync became /device when the page grew from a protocol console into a
// dashboard. Kept as a server-rendered 308 so old bookmarks still land
// somewhere useful; next.config.ts redirects would work too, but this keeps
// the rename self-contained in the routes it concerns.
export default function SyncRedirect() {
  permanentRedirect("/device");
}
