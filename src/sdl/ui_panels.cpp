// src/sdl/ui_panels.cpp — SPDX-License-Identifier: MIT
//
// Directory-tree + file-list ImGui panel. Replaces src/dirtree.c (a
// GtkTreeView) and src/filelist.c (a GtkTreeView used as a flat list) --
// together, the left-hand pane src/window.c builds via gui_hpaned_add()/
// gui_vpaned_add(). Neither GTK file is touched; this is the SDL-only
// counterpart, and it is what turns most of src/sdl/stubs.c's dirtree_*/
// filelist_* no-ops (see that file's Task 5.2 TODO note) into the real
// thing -- those symbols are DEFINED HERE now, not in stubs.c.
//
// ---- Why this file, not a persistent widget tree -------------------
//
// dirtree.c owns a GtkTreeStore: an explicit row per directory, built
// incrementally as scanfs.c calls dirtree_entry_new(), and mutated by
// dirtree_entry_expand()/_collapse_recursive() as the user (or the 3D
// view's context menu) opens/closes directories. This file has no such
// persistent structure: draw_dir_node() below walks the *live* GNode
// tree directly, every frame, descending only into directories already
// known to be open. That is what makes "iterate only visible nodes"
// (the brief's perf requirement) fall out for free from ImGui's own
// TreeNodeEx()/lazy-children idiom, with no separate model to keep in
// sync -- there is only ever one source of truth, the GNode tree itself.
//
// The one piece of *persistent* per-directory state this approach still
// needs -- "is this directory's row open" -- reuses DirNodeDesc::tnode
// (src/common.h), the exact field dirtree.c uses to remember its
// GtkTreePath for the same node. scanfs.c never touches that field
// itself except to NULL it once before the very first dirtree_entry_new()
// call (src/scanfs.c:333, "needed in dirtree_entry_new()"), so
// repurposing it as a plain boolean here needs no core change: see
// tree_row_expanded()/set_tree_row_expanded() below.
//
// ---- GTK -> ImGui cross-reference (see docs/PORTING.md for the full
// writeup) ----
//
//   dirtree.c's dirtree_select_cb()   -> draw_dir_node()'s click branch
//   dirtree.c's dirtree_expand_cb()/
//     dirtree_collapse_cb()          -> draw_dir_node()'s IsItemToggledOpen() branch
//   dirtree_entry_new/_show/_expand*/
//     _collapse_recursive/_expanded  -> real implementations below (were
//                                        src/sdl/stubs.c no-ops)
//   filelist.c's filelist_populate() -> populate_file_list() (static;
//                                        never part of the core contract --
//                                        only dirtree.c/filelist.c called
//                                        the original)
//   filelist.c's filelist_select_cb()-> draw_file_list_section()'s Selectable
//   filelist_show_entry()/
//     filelist_reset_access()        -> real / no-op below, see each
//   window.c's hpaned_w/vpaned_w      -> a single ImGui window, tree on
//                                        top (~1/3 height, matching
//                                        vpaned_w's window_height/3
//                                        initial split) and file list
//                                        below (~2/3) -- brief's
//                                        "simplest: one panel" option
#include "ui_panels.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
// DockBuilder* and ImGuiDockNode are "very early end-user API ... expect
// this to change/break" (imgui.cpp's own header above the Docking:
// Builder Functions section) -- deliberately not in imgui.h, only here,
// only for the one-time default-layout seed in ui_dockspace_draw() below.
#include <imgui_internal.h>

#include "app.h"
#include "ui_dialogs.h"

extern "C" {
#include "common.h"
#include "camera.h"
#include "colexp.h"
#include "dirtree.h"
#include "filelist.h"
#include "geometry.h"
#include "window.h"
}

// ---- Expansion-state storage ---------------------------------------
//
// See the file header: DirNodeDesc::tnode, repurposed as a 0/1 "row
// open" flag instead of a GtkTreePath. NULL/non-NULL rather than a
// dedicated bool type so no header (src/common.h) needs touching.
// tools/fsv-headless-stubs.c mirrors these exact semantics for the
// headless unit tests (test_fsn_camera, test_fsn_layout) -- keep the
// two in sync.
static inline bool
tree_row_expanded(GNode *dnode)
{
	return DIR_NODE_DESC(dnode)->tnode != nullptr;
}

static inline void
set_tree_row_expanded(GNode *dnode, bool expanded)
{
	DIR_NODE_DESC(dnode)->tnode = expanded ? reinterpret_cast<void *>(1) : nullptr;
}

// Port of gtk_tree_view_expand_row(path, open_all=TRUE): opens dnode and
// every directory beneath it. Only ever called for an explicit "expand
// all" action (colexp.c's COLEXP_EXPAND_RECURSIVE, wired from dialog.c's
// Properties dialog -- Task 5.3), never per-frame, so its O(subtree)
// cost is the same one-shot cost GTK pays materializing the same rows.
static void
expand_subtree_recursive(GNode *dnode)
{
	set_tree_row_expanded(dnode, true);
	for (GNode *c = dnode->children; c != nullptr; c = c->next) {
		if (!NODE_IS_DIR(c))
			break; // dirs always sort first -- scanfs.c's compare_node
		expand_subtree_recursive(c);
	}
}

// Opens every ancestor directory of dnode, so dnode itself becomes
// reachable by draw_dir_node()'s top-down walk. Needed here in a way it
// wasn't for GTK's model/view split: GtkTreeStore rows all exist
// regardless of expansion (collapsing a row just hides its children),
// but this file's walk *only* descends into rows it already knows are
// open, so an ancestor left closed would make dnode undrawable no
// matter what its own flag says.
static void
expand_ancestors(GNode *dnode)
{
	for (GNode *p = dnode->parent; p != nullptr && NODE_IS_DIR(p); p = p->parent)
		set_tree_row_expanded(p, true);
}

// ---- Shared dir/file-list state -------------------------------------
//
// One set of statics for what were two separate GTK files' worth of
// per-widget state (dirtree.c's dirtree_current_dnode, filelist.c's
// filelist_current_dnode) -- both moved in lockstep in the original
// anyway (dirtree.c always called filelist_populate() then set its own
// current-dnode right after), so one name each suffices here.

// Directory currently shown in the file list / selected in the tree --
// dirtree.c's dirtree_current_dnode + filelist.c's filelist_current_dnode.
static GNode *g_shown_dir = nullptr;

// Best-effort tree scroll target, consumed (if reachable at all -- see
// dirtree_entry_show() below) the next time it is actually drawn.
static GNode *g_dirtree_scroll_to = nullptr;

// The specific file/dir row highlighted in the file list -- filelist.c's
// own TreeSelection state, distinct from g_shown_dir (the *directory*
// whose contents are listed).
static GNode *g_filelist_selected = nullptr;
static GNode *g_filelist_scroll_to = nullptr;

// Cached, alphabetically-sorted contents of g_shown_dir, rebuilt only
// when the shown directory changes (populate_file_list() below) --
// exactly as expensive as filelist.c's own filelist_populate(), which
// likewise only re-sorts on a directory change, not every frame.
static std::vector<GNode *> g_file_list;

static bool g_panels_visible = true;

// Shared between ui_dockspace_draw() (DockBuilderDockWindow() needs the
// exact title string to pre-dock by) and ui_panels_draw() (Begin()'s own
// title) -- one constant so the two can never drift apart.
static const char *const kPanelWindowTitle = "Directory Tree";

// Port of filelist.c's compare_node(): alphabetical by name. Unlike the
// dir tree below, this is cheap to apply here because it only runs when
// the shown directory changes (see the cache above), not per frame.
static bool
compare_name(GNode *a, GNode *b)
{
	return strcmp(NODE_DESC(a)->name, NODE_DESC(b)->name) < 0;
}

// Rebuilds the file-list cache for dnode's immediate children. Not
// exposed as filelist_populate(): that symbol is filelist.h's, but
// nothing outside dirtree.c/filelist.c ever called it (grep confirms),
// so there is no core-facing contract to preserve under that name here.
static void
populate_file_list(GNode *dnode)
{
	g_file_list.clear();
	for (GNode *c = dnode->children; c != nullptr; c = c->next)
		g_file_list.push_back(c);
	std::sort(g_file_list.begin(), g_file_list.end(), compare_name);

	// Node-count message in the left statusbar -- filelist.c's own
	// count message, ported verbatim (minus gettext: this frontend
	// wires up no NLS, matching ui_main.cpp's plain-English strings).
	char buf[64];
	const size_t n = g_file_list.size();
	if (n == 0)
		buf[0] = '\0';
	else if (n == 1)
		SDL_strlcpy(buf, "1 node", sizeof(buf));
	else
		SDL_snprintf(buf, sizeof(buf), "%zu nodes", n);
	window_statusbar(SB_LEFT, buf);

	g_shown_dir = dnode;
}

// ---- dirtree.h: real implementations (were src/sdl/stubs.c no-ops) --

extern "C" void
dirtree_clear(void)
{
	g_shown_dir = nullptr;
	g_dirtree_scroll_to = nullptr;
	g_filelist_selected = nullptr;
	g_filelist_scroll_to = nullptr;
	g_file_list.clear();

	// Task 5.3: the Properties window (src/sdl/ui_dialogs.cpp) can hold
	// a target-symlink GNode* across many frames -- unlike this file's
	// own pointers above, which every draw call re-derives from
	// g_shown_dir/scanfs.c's fresh tree, a Properties window can sit
	// open, untouched, through an entire Change Root/Rescan. Force it
	// closed here, at the exact point (scanfs.c's call to this
	// function, before it tears down the old tree) the rest of this
	// file already resets its own now-stale pointers for.
	ui_dialogs_close_properties();
}

extern "C" void
dirtree_entry_new(GNode *dnode)
{
	// Port of dirtree.c's dirtree_entry_new(): a directory starts open
	// iff its GNode depth is <= 2, i.e. only the scanned root itself
	// (globals.fstree, the metanode, is depth 1; root_dnode is depth 2;
	// root's own subdirectories are depth 3 and start closed). Matches
	// the pre-Task-5.2 stub's dnode == root_dnode special case exactly.
	set_tree_row_expanded(dnode, g_node_depth(dnode) <= 2);
}

extern "C" void
dirtree_no_more_entries(void)
{
	// Nothing to finalize. dirtree.c's own version is a TODO/no-op
	// today (re-attaching a GtkTreeModel this file never detached in
	// the first place) -- there is no persistent widget model here at
	// all (see the file header), so there is nothing to reattach.
}

extern "C" boolean
dirtree_entry_expanded(GNode *dnode)
{
	if (dnode == nullptr)
		return FALSE;
	return tree_row_expanded(dnode) ? TRUE : FALSE;
}

extern "C" void
dirtree_entry_collapse_recursive(GNode *dnode)
{
	if (dnode == nullptr)
		return;
	// Port of dirtree.c: gtk_tree_view_collapse_row() collapses exactly
	// the one row -- it does not touch descendants' own expanded state,
	// which simply stops being reachable while this row is closed and
	// reappears exactly as it was if this row reopens. Matched here by
	// only ever clearing dnode's own flag, never recursing.
	set_tree_row_expanded(dnode, false);
}

extern "C" void
dirtree_entry_expand(GNode *dnode)
{
	if (dnode == nullptr)
		return;
	// Port of dirtree.c: gtk_tree_view_expand_to_path(). See
	// expand_ancestors()'s doc comment for why the ancestor walk is
	// necessary here even though GTK's own version needed no equivalent.
	set_tree_row_expanded(dnode, true);
	expand_ancestors(dnode);
}

extern "C" void
dirtree_entry_expand_recursive(GNode *dnode)
{
	if (dnode == nullptr)
		return;
	// Port of dirtree.c: gtk_tree_view_expand_row(path, TRUE).
	expand_subtree_recursive(dnode);
	expand_ancestors(dnode);
}

// dirtree_entry_show(): the other half of filelist_show_entry() below
// (dirtree.c keeps it as a separate public entry point; nothing else
// calls it directly here, but it stays a distinct function to mirror
// dirtree.c's own split and because colexp() calls its GTK counterpart
// under a different name -- keeping the same shape makes the two files
// easy to diff against each other).
extern "C" void
dirtree_entry_show(GNode *dnode)
{
	if (dnode != g_shown_dir)
		populate_file_list(dnode); // also sets g_shown_dir
	g_shown_dir = dnode; // unconditional, matches dirtree.c's own tail assignment
	g_dirtree_scroll_to = dnode; // best-effort: see draw_dir_node()
}

// ---- filelist.h: real implementations (were src/sdl/stubs.c no-ops) -

extern "C" void
filelist_reset_access(void)
{
	// dirtree.c's version caches "is the shown directory expanded" into
	// the file-list widget's sensitivity right away. This file instead
	// re-reads dirtree_entry_expanded(g_shown_dir) live every time the
	// file list draws (draw_file_list_section() below) -- an ImGui
	// window has no persistent "insensitive" state to push a value
	// into ahead of time the way a GTK widget does, so there is nothing
	// useful for this notification to do.
}

extern "C" void
filelist_scan_monitor_init(void)
{
	// No-op: src/sdl/main.cpp's gui_update() already draws its own
	// scan-progress overlay (a plain ImGui window, not this panel --
	// see main.cpp's Task 5.2 comment), which is what SB_LEFT/SB_RIGHT
	// statusbar text and filelist_scan_monitor() below would otherwise
	// feed. This panel itself is hidden throughout the scan (see
	// ui_panels_draw()), so there is no scan-monitor UI here to init.
}

extern "C" void
filelist_scan_monitor(int *node_counts, int64 *size_counts)
{
	(void)node_counts;
	(void)size_counts;
}

// The one real, meaningful core -> UI notification besides the dirtree_
// family above: called from src/camera.c's post_pan_end() after *every*
// completed camera pan, and from src/sdl/input.cpp's right-click branch
// (Task 4.1/4.2) -- both already wired to call this before Task 5.2
// existed, since it was a "legitimate (if currently no-op) entry point"
// per stubs.c's own comment.
extern "C" void
filelist_show_entry(GNode *node)
{
	if (node == nullptr)
		return;

	GNode *dnode = NODE_IS_DIR(node) ? node : node->parent;
	if (dnode != g_shown_dir)
		dirtree_entry_show(dnode);

	g_filelist_selected = node;
	g_filelist_scroll_to = node;
}

// ---- Drawing: directory tree -----------------------------------------

static void
draw_dir_node(GNode *dnode)
{
	// NODE_DNAME(), not NODE_DESC(dnode)->name: NFC-normalized, guaranteed-valid UTF-8
	// (see scanfs.c's display_name()) -- ImGui draws UTF-8 directly and
	// has no combining-mark composition of its own.
	const char *dname = NODE_DNAME(dnode);
	const char *name = (dname != nullptr && dname[0] != '\0') ?
	    dname : "/ (root)"; // dirtree.c's own empty-name fallback

	// Dirs sort before every other type (scanfs.c's compare_node, "must
	// always go before leafs" -- its own comment), so both this check
	// and the recursion below can stop at the first non-dir child.
	bool has_subdirs = false;
	for (GNode *c = dnode->children; c != nullptr; c = c->next) {
		if (!NODE_IS_DIR(c))
			break;
		has_subdirs = true;
		break;
	}

	const bool was_open = tree_row_expanded(dnode);
	ImGui::SetNextItemOpen(was_open, ImGuiCond_Always);

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
	    ImGuiTreeNodeFlags_SpanAvailWidth;
	if (dnode == g_shown_dir)
		flags |= ImGuiTreeNodeFlags_Selected; // dirtree.c's TreeSelection state
	if (!has_subdirs)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

	// Best-effort scroll (dirtree_entry_show()'s "scroll directory tree
	// to proper entry"): only takes effect if this row is actually
	// being drawn this frame, i.e. every ancestor is already open.
	// Matches GTK's own non-forcing behavior -- gtk_tree_selection_
	// select_path() does not expand collapsed ancestors either.
	if (dnode == g_dirtree_scroll_to) {
		ImGui::SetScrollHereY();
		g_dirtree_scroll_to = nullptr;
	}

	const bool node_open = ImGui::TreeNodeEx(
	    static_cast<void *>(dnode), flags, "%s", name);
	const bool toggled = has_subdirs && ImGui::IsItemToggledOpen();

	if (toggled) {
		// Port of dirtree.c's dirtree_expand_cb()/dirtree_collapse_cb()
		// (its "row_expanded"/"row_collapsed" signal handlers).
		if (node_open && !was_open)
			colexp(dnode, COLEXP_EXPAND);
		else if (!node_open && was_open)
			colexp(dnode, COLEXP_COLLAPSE_RECURSIVE);
	} else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
		// Port of dirtree.c's dirtree_select_cb(): an already-open row
		// flies the camera there (its contents are already visible in
		// the 3D scene); a closed row instead previews its contents in
		// the file list below, without moving the camera. Exactly
		// dirtree_select_cb()'s own asymmetric branching -- not a
		// simplification of it.
		if (was_open) {
			camera_look_at(dnode);
		} else {
			geometry_highlight_node(dnode, FALSE);
			window_statusbar(SB_RIGHT, node_absname_display(dnode));
			if (dnode != g_shown_dir)
				populate_file_list(dnode);
		}
	}

	// ImGui::TreePop() must be called if and only if TreeNodeEx() pushed
	// an ID scope for this node, which it only does when node_open is
	// true (imgui_widgets.cpp: `if (is_open && !NoTreePushOnOpen)
	// TreePushOverrideID(id);`) -- calling it unconditionally whenever
	// has_subdirs is true, regardless of node_open, unbalances the ID
	// stack the moment a node with children is *collapsed* via a real
	// click and asserts inside the next TreePop() (window->IDStack.Size
	// > 1) -- caught by Task 5.2's own verification harness collapsing
	// root_dnode via its arrow, not a hypothetical.
	if (has_subdirs && node_open) {
		for (GNode *c = dnode->children; c != nullptr; c = c->next) {
			if (!NODE_IS_DIR(c))
				break;
			draw_dir_node(c);
		}
		ImGui::TreePop();
	}
}

// ---- Drawing: file list -----------------------------------------------

static const char *
node_type_tag(NodeType t)
{
	// Text prefix replacing filelist.c's mini pixmap column (the brief's
	// "type icon-as-text prefix"), one glyph-ish tag per NodeType.
	switch (t) {
	case NODE_DIRECTORY: return "[DIR]";
	case NODE_REGFILE:   return "[FILE]";
	case NODE_SYMLINK:   return "[LINK]";
	case NODE_FIFO:      return "[FIFO]";
	case NODE_SOCKET:    return "[SOCK]";
	case NODE_CHARDEV:   return "[CHR]";
	case NODE_BLOCKDEV:  return "[BLK]";
	default:             return "[?]";
	}
}

static void
draw_file_list_section(void)
{
	ImGui::TextUnformatted(g_shown_dir != nullptr ?
	    node_absname_display(g_shown_dir) : "(no directory selected)");

	// Port of filelist.c's filelist_reset_access(): the file list is
	// only meaningfully browsable while its directory's contents are
	// actually visible in the 3D scene, i.e. while that row is open.
	const bool enabled = g_shown_dir != nullptr &&
	    tree_row_expanded(g_shown_dir);
	if (!enabled)
		g_filelist_selected = nullptr; // gtk_tree_selection_unselect_all()

	ImGui::BeginDisabled(!enabled);
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (ImGui::BeginTable("filelist", 3,
	    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
	    avail)) {
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 52.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 72.0f);
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_file_list.size()));
		// IncludeItemByIndex() *before* the first Step() (imgui.h's own
		// doc comment on it) is required for a pending scroll-to onto a
		// row outside the currently visible range: the clipper would
		// otherwise never hand this row's index to the loop below at
		// all (it is clipped away, not merely drawn off-screen), so the
		// child == g_filelist_scroll_to check inside the loop can never
		// see it and SetScrollHereY() never fires. Exactly the bug
		// filelist_show_entry() hits every time camera.c's
		// post_pan_end() (or a right-click) lands on a file outside the
		// list's current scroll position -- e.g. any entry past the
		// first screenful of a large directory like /opt/homebrew/bin.
		if (g_filelist_scroll_to != nullptr) {
			auto it = std::find(g_file_list.begin(), g_file_list.end(),
			    g_filelist_scroll_to);
			if (it != g_file_list.end())
				clipper.IncludeItemByIndex(
				    static_cast<int>(it - g_file_list.begin()));
			else
				// Not a row of *this* list -- e.g. filelist_show_entry()
				// was called with the shown directory itself (never a
				// row within its own listing), which is exactly what
				// happens once per load right after the intro camera
				// pan settles on root_dnode. Clear it now rather than
				// leaving a stale pointer for every future frame's
				// std::find() to rescan for nothing, forever.
				g_filelist_scroll_to = nullptr;
		}
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
				GNode *child = g_file_list[i];
				const NodeDesc *nd = NODE_DESC(child);

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(node_type_tag(nd->type));

				ImGui::TableNextColumn();
				const bool selected = (child == g_filelist_selected);
				ImGui::PushID(child);
				if (ImGui::Selectable(NODE_DNAME(child), selected,
				    ImGuiSelectableFlags_SpanAllColumns)) {
					// Port of filelist.c's filelist_select_cb(): every
					// click -- file or directory -- flies the camera
					// there (unlike the tree above, which only does
					// this for already-open directories).
					camera_look_at(child);
					geometry_highlight_node(child, FALSE);
					window_statusbar(SB_RIGHT, node_absname_display(child));
					g_filelist_selected = child;
				}
				ImGui::PopID();

				ImGui::TableNextColumn();
				const int64 size = NODE_IS_DIR(child) ?
				    DIR_NODE_DESC(child)->subtree.size : nd->size;
				ImGui::TextUnformatted(abbrev_size(size));

				if (child == g_filelist_scroll_to) {
					ImGui::SetScrollHereY();
					g_filelist_scroll_to = nullptr;
				}
			}
		}
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
}

// ---- Top-level panel ---------------------------------------------------

bool
ui_panels_get_visible(void)
{
	return g_panels_visible;
}

void
ui_panels_set_visible(bool visible)
{
	g_panels_visible = visible;
}

// Submits the passthrough dockspace every frame (ImGui's own docs:
// "Dockspaces need to be submitted _before_ any window they can host.
// Submit them early in your frame!"), and, once only, seeds a default
// left-docked layout if -- and only if -- no layout was already
// restored from imgui.ini for this dockspace. Called from main.cpp
// after ui_main_draw() (so ImGui::GetMainViewport()'s WorkPos/WorkSize
// already reflects this frame's main-menu-bar shrink -- ui_main_draw()
// itself never docks anything, so running it first costs nothing) and
// before ui_panels_draw() (which does).
void
ui_dockspace_draw(void)
{
	const ImGuiViewport *vp = ImGui::GetMainViewport();

	// ImGuiDockNodeFlags_PassthruCentralNode: the empty central node (no
	// window ever gets docked there -- only the left split, below, is
	// ever targeted) gets a *real* input hit-test hole punched through
	// it (imgui.cpp's DockNodeUpdate(): SetWindowHitTestHole()), not
	// merely a transparent background. That is what keeps the 3D scene
	// receiving clicks/drags there -- WantCaptureMouse stays false over
	// it exactly as it did before this dockspace existed.
	const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
	    0, vp, ImGuiDockNodeFlags_PassthruCentralNode);

	static bool first_frame = true;
	if (!first_frame)
		return;
	first_frame = false;

	// ImGui loads imgui.ini's saved dock-node structure (splits, which
	// windows are docked where) at CreateContext()/first-NewFrame() time
	// -- before this ever runs. So if this exact dockspace ID was saved
	// with a real layout in a previous run, the node DockSpaceOverViewport()
	// just resolved above already carries that structure (it is split
	// and/or already hosts windows) by the time we get here. A node
	// that is still IsEmpty() (leaf, no docked window) has nothing to
	// preserve -- either imgui.ini didn't exist yet, or it never saved
	// this dockspace -- so seed one sensible default layout: the panel
	// docked into a left ~25% split, matching window.c's own hpaned_w
	// initial ratio (window_width/5), everything else left as the
	// passthrough central node for the 3D scene.
	ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace_id);
	if (node == nullptr || !node->IsEmpty())
		return;

	ImGuiID dock_left = 0, dock_main = 0;
	ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.25f,
	    &dock_left, &dock_main);
	ImGui::DockBuilderDockWindow(kPanelWindowTitle, dock_left);
	ImGui::DockBuilderFinish(dockspace_id);
}

void
ui_panels_draw(void)
{
	if (!g_panels_visible)
		return;
	// Scan-time: hidden outright, not merely greyed out -- matches how
	// ui_main.cpp's menu bar (Task 5.1) is not drawn at all during a
	// deferred Rescan/Change Root, for the same reason (no stable
	// globals.fstree to read: scanfs.c's dirtree_clear(), which this
	// file's dirtree_clear() above answers, runs at the very start of
	// every scan). globals.fsv_mode == FSV_NONE covers the brief window
	// between load_filesystem()'s wipe and its enter_mode() call.
	if (app_is_scanning() || globals.fsv_mode == FSV_NONE)
		return;
	if (globals.fstree == nullptr || root_dnode == nullptr)
		return;

	// Initial placement only (ImGuiCond_FirstUseEver): a manual resize
	// persists across frames afterward, same as any ordinary ImGui
	// window. Ratios match window.c's own initial paned-widget split --
	// hpaned_w's window_width/5 for this panel's width against the 3D
	// viewport, vpaned_w's window_height/3 for the tree's share of this
	// panel's own height (the file list gets the remaining ~2/3).
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(
	    ImVec2(vp->WorkSize.x / 5.0f, vp->WorkSize.y), ImGuiCond_FirstUseEver);

	if (ImGui::Begin(kPanelWindowTitle, &g_panels_visible)) {
		const float avail_h = ImGui::GetContentRegionAvail().y;
		ImGui::BeginChild("##dirtree_scroll", ImVec2(0.0f, avail_h / 3.0f),
		    ImGuiChildFlags_Borders);
		draw_dir_node(root_dnode);
		ImGui::EndChild();

		ImGui::Separator();

		draw_file_list_section();
	}
	ImGui::End();
}
