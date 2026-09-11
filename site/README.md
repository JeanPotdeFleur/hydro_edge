# Hopkins rocky-shore camera station

**Status page: https://JeanPotdeFleur.github.io/hopkins-station/**

Open that page. It shows the view from the station as it was at 08:00 this
morning, whether the station is running, and what it is scheduled to acquire
today. Reading it needs no account.

## The only file to edit

`control/plan.json` sets today's acquisition. Nothing else here is meant to be
changed by hand, and the button on the status page opens this file directly.

```json
{
  "date": "2026-09-14",
  "bursts": 2,
  "exposure_us": 2200,
  "note": "low tide at 08:10, keeping the morning slot"
}
```

| Field | What it does |
| --- | --- |
| `date` | Today's date, Pacific time. The plan is ignored unless this matches, so a plan nobody updated expires on its own. |
| `bursts` | How many 40-minute bursts to acquire: 0, 1, 2 or 3. One is 10:00; two is 10:00 and 17:00; three is 10:00, 13:00 and 17:00. |
| `exposure_us` | Optional. Exposure in microseconds, between 100 and 20000. Leave it out to keep the station's own setting. |
| `note` | Optional free text. It is copied into the archive beside the frames. |

Commit before **09:45 Pacific** for the change to apply the same day. The
station reads the plan at 09:50 and does not look again, so a burst already
under way is never altered by a late edit.

If any field is missing, out of range or the file will not parse, the station
acquires two bursts at 10:00 and 17:00 rather than nothing at all. The reason
it fell back is shown on the status page.

## What the station writes here

`latest.jpg` and `status.json`, once each morning, both overwritten. Do not
edit them.

`index.html` is the status page. Its source of truth is `site/index.html` in
the [hydro_edge](https://github.com/JeanPotdeFleur/hydro_edge) repository: if
it is ever broken here, copy it back from there.
