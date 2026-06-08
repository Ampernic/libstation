/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* macOS backend: an NSStatusItem in the menu bar with an NSMenu built from the
 * item store. Lives in the GTK process (its quartz backend pumps the AppKit run
 * loop). No ARC — manual retain/release. */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-tray-private.h"
#import <Cocoa/Cocoa.h>

@interface StationTrayTarget : NSObject
{
@public
  StationTray *tray;
}
- (void) itemClicked: (id) sender;
@end

@implementation StationTrayTarget
- (void) itemClicked: (id) sender
{
  _station_tray_emit_activated (tray, (guint) [(NSMenuItem *) sender tag]);
}
@end

typedef struct
{
  NSStatusItem      *item;
  StationTrayTarget *target;
} Mac;

static NSString *
ns (const char *s)
{
  return [NSString stringWithUTF8String: (s ? s : "")];
}

static void
rebuild_menu (StationTray *self)
{
  Mac *m = self->backend;
  NSMenu *menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems: NO];

  guint n = _station_tray_item_count (self);
  for (guint i = 0; i < n; i++)
    {
      const StationTrayItem *it = _station_tray_item_at (self, i);
      if (it->id == 0)
        {
          [menu addItem: [NSMenuItem separatorItem]];
          continue;
        }
      NSMenuItem *mi = [[NSMenuItem alloc] initWithTitle: ns (it->label)
                        action: @selector (itemClicked:) keyEquivalent: @""];
      [mi setTarget: m->target];
      [mi setTag: (NSInteger) it->id];
      [mi setEnabled: it->enabled ? YES : NO];
      [menu addItem: mi];
      [mi release];
    }
  [m->item setMenu: menu];
  [menu release];
}

void
_station_tray_backend_init (StationTray *self)
{
  Mac *m = g_new0 (Mac, 1);
  self->backend = m;
  m->target = [[StationTrayTarget alloc] init];
  m->target->tray = self;
  m->item = [[[NSStatusBar systemStatusBar]
              statusItemWithLength: NSVariableStatusItemLength] retain];
  _station_tray_backend_update (self);
}

void
_station_tray_backend_update (StationTray *self)
{
  Mac *m = self->backend;
  if (m == NULL || m->item == nil)
    return;

  if (self->icon_file != NULL)
    {
      NSImage *img = [[NSImage alloc] initWithContentsOfFile: ns (self->icon_file)];
      if (img != nil)
        {
          [img setTemplate: YES];   /* tint to the menu-bar appearance */
          [[m->item button] setImage: img];
          [img release];
        }
    }
  [[m->item button] setToolTip: ns (self->tooltip)];
  rebuild_menu (self);
}

void
_station_tray_backend_dispose (StationTray *self)
{
  Mac *m = self->backend;
  if (m == NULL)
    return;
  if (m->item != nil)
    {
      [[NSStatusBar systemStatusBar] removeStatusItem: m->item];
      [m->item release];
    }
  [m->target release];
  g_free (m);
  self->backend = NULL;
}
