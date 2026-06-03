#pragma once

#include <string>

namespace ae {

// Reveal a freshly placed top-level item in File Explorer, pre-selected.
//
// Given the absolute path of the single placed top-level item (a file, an
// unwrapped folder, or the wrapper folder produced by the layout planner), this
// opens File Explorer on the item's PARENT directory (the target working dir)
// with the item selected and the window brought to the foreground.
//
// Strategy (see design 04-shell-integration §Reveal in Explorer):
//   1. Reuse path (preferred): enumerate open Explorer windows via
//      IShellWindows; if one already shows the target working directory, select
//      the new item in that window and bring it to the front.
//   2. Fallback: SHOpenFolderAndSelectItems with the working-dir PIDL and the
//      new item as the selection (opens or focuses a window with the item
//      selected).
//
// THREADING: must be called on a thread with COM initialized as an STA
// (CoInitializeEx with COINIT_APARTMENTTHREADED). In this app that is the UI
// thread, which initializes COM at startup. If COM is not already initialized on
// the calling thread this function initializes it for the duration of the call.
//
// `placedPath` empty (empty-archive / nothing placed) is a no-op. Never called
// on cancel or error. Returns true if a window was reused or opened.
bool RevealInExplorer(const std::wstring& placedPath);

}  // namespace ae
