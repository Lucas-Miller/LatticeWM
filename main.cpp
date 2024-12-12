#include <windows.h>
#include <vector>
#include <iostream>
#include <memory>
#include <functional>
#include <algorithm>
#include <queue>

// Define MOD key (can be changed to MOD_CONTROL, MOD_WIN, etc.)
const UINT MOD_KEY = MOD_ALT;

// Enumeration for split orientation
enum class SplitType {
    VERTICAL,
    HORIZONTAL
};

// Enumeration for navigation directions
enum class Direction {
    UP,
    DOWN,
    LEFT,
    RIGHT
};

// Structure to hold window information for fullscreen toggling
struct WindowInfo {
    HWND hwnd;
    RECT savedRect;            // Saved original position and size for fullscreen toggle
    LONG savedStyle;           // Saved original style for fullscreen toggle
    bool isFullscreen = false; // Track fullscreen state
};

// Structure to represent each node in the layout tree
struct LayoutNode {
    // Indicates whether this node is a split or a leaf (window)
    bool isSplit;

    // Split orientation (valid only if isSplit is true)
    SplitType splitType;

    // Split ratio (e.g., 0.5 for equal split)
    float splitRatio;

    // Child nodes (valid only if isSplit is true)
    std::unique_ptr<LayoutNode> firstChild;
    std::unique_ptr<LayoutNode> secondChild;

    // Parent node pointer (useful for traversal)
    LayoutNode* parent;

    // Window information (valid only if isSplit is false)
    WindowInfo windowInfo;

    // Pointers to adjacent nodes
    LayoutNode* left = nullptr;
    LayoutNode* right = nullptr;
    LayoutNode* up = nullptr;
    LayoutNode* down = nullptr;

    // Rectangle representing window position and size
    RECT windowRect;

    // Constructors
    // For leaf nodes
    LayoutNode(HWND window) 
        : isSplit(false), splitType(SplitType::VERTICAL), splitRatio(0.5f),
          firstChild(nullptr), secondChild(nullptr), parent(nullptr), 
          windowInfo{window, RECT{}, 0, false} {}

    // For split nodes
    LayoutNode(SplitType type, float ratio,
               std::unique_ptr<LayoutNode> first,
               std::unique_ptr<LayoutNode> second)
        : isSplit(true), splitType(type), splitRatio(ratio),
          firstChild(std::move(first)), secondChild(std::move(second)),
          parent(nullptr), windowInfo{nullptr, RECT{}, 0, false} {}
};

// Structure to hold queue items with node and its depth
struct QueueItem {
    LayoutNode* node;
    int depth;
};


// Global root of the layout tree
std::unique_ptr<LayoutNode> root;

// Vector to store all managed windows
std::vector<WindowInfo> managedWindows;

// Function to normalize and move windows
void MoveWindowNormalized(HWND hwnd, int x, int y, int width, int height) {
    if (!hwnd) return;

    // Retrieve original style
    LONG originalStyle = GetWindowLong(hwnd, GWL_STYLE);

    // Temporarily set to a normalized style
    SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // Move the window
    if (!SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOZORDER | SWP_SHOWWINDOW)) {
        std::cerr << "Failed to move window. Error: " << GetLastError() << "\n";
    }

    // Restore original style
    SetWindowLong(hwnd, GWL_STYLE, originalStyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

// Callback to collect visible windows that will be managed
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE; // Skip invisible windows
    if (GetWindowTextLength(hwnd) == 0) return TRUE; // Skip untitled windows
    LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW) return TRUE; // Skip tool windows
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if ((style & WS_POPUP) || (style & WS_CHILD)) return TRUE; // Skip popups or child windows

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) {
        std::cerr << "Failed to retrieve RECT for hwnd=" << hwnd << ". Error: " << GetLastError() << "\n";
        return TRUE;
    }

    if (rect.left == rect.right || rect.top == rect.bottom) return TRUE; // Skip windows with no area

    // Initialize WindowInfo and add to managedWindows
    WindowInfo winInfo;
    winInfo.hwnd = hwnd;
    winInfo.savedRect = rect; // Initially set to current rect
    winInfo.savedStyle = GetWindowLong(hwnd, GWL_STYLE);
    windows->push_back(winInfo);
    return TRUE;
}

// Function to initialize the layout with the first window
void InitializeLayout(HWND firstWindow) {
    root = std::make_unique<LayoutNode>(firstWindow);
}

// Function to add a new window using breadth-first split with depth tracking
// Function to add a new window using breadth-first split strategy with depth tracking
void AddWindowBreadthFirst(HWND newWindow, float splitRatio = 0.5f) {
    if (!root) {
        // If root is not initialized, initialize with the new window
        root = std::make_unique<LayoutNode>(newWindow);
        return;
    }

    // Use a queue to perform breadth-first traversal with depth tracking
    std::queue<QueueItem> nodeQueue;
    nodeQueue.push(QueueItem{ root.get(), 0 }); // Root node at depth 0

    while (!nodeQueue.empty()) {
        QueueItem currentItem = nodeQueue.front();
        nodeQueue.pop();

        LayoutNode* current = currentItem.node;
        int depth = currentItem.depth;

        if (!current->isSplit) {
            // Split this leaf node
            current->isSplit = true;

            // Determine split type based on depth
            SplitType splitType = (depth % 2 == 0) ? SplitType::HORIZONTAL : SplitType::VERTICAL;
            current->splitType = splitType;
            current->splitRatio = splitRatio;

            // Create child nodes
            current->firstChild = std::make_unique<LayoutNode>(current->windowInfo.hwnd);
            current->firstChild->parent = current;
            current->secondChild = std::make_unique<LayoutNode>(newWindow);
            current->secondChild->parent = current;

            // Clear the window handle in the split node
            current->windowInfo.hwnd = nullptr;

            // Enqueue child nodes with incremented depth
            nodeQueue.push(QueueItem{ current->firstChild.get(), depth + 1 });
            nodeQueue.push(QueueItem{ current->secondChild.get(), depth + 1 });

            return; // Window added successfully
        }
        else {
            // If it's a split node, enqueue its children with incremented depth
            if (current->firstChild) {
                nodeQueue.push(QueueItem{ current->firstChild.get(), depth + 1 });
            }
            if (current->secondChild) {
                nodeQueue.push(QueueItem{ current->secondChild.get(), depth + 1 });
            }
        }
    }
}


// Function to establish adjacencies by setting left, right, up, down pointers
void EstablishAdjacencies(LayoutNode* node) {
    if (!node) return;

    if (!node->isSplit) {
        // Leaf node, no adjacencies to establish here
        return;
    }

    // Recursively establish adjacencies for child nodes first
    EstablishAdjacencies(node->firstChild.get());
    EstablishAdjacencies(node->secondChild.get());

    LayoutNode* first = node->firstChild.get();
    LayoutNode* second = node->secondChild.get();

    // Depending on the split type, establish adjacencies
    if (node->splitType == SplitType::VERTICAL) {
        // Vertical split: firstChild is to the left, secondChild is to the right

        // Collect leaf nodes in firstChild and secondChild
        std::vector<LayoutNode*> firstLeaves;
        std::vector<LayoutNode*> secondLeaves;

        std::function<void(LayoutNode*)> collectLeaves = [&](LayoutNode* n) {
            if (!n->isSplit && n->windowInfo.hwnd != nullptr) {
                firstLeaves.push_back(n);
            }
            else {
                if (n->firstChild) collectLeaves(n->firstChild.get());
                if (n->secondChild) collectLeaves(n->secondChild.get());
            }
        };

        collectLeaves(first);
        collectLeaves(second);

        // Map adjacencies: left windows' right to right windows
        size_t count = std::min(firstLeaves.size(), secondLeaves.size());
        for (size_t i = 0; i < count; ++i) {
            firstLeaves[i]->right = secondLeaves[i];
            secondLeaves[i]->left = firstLeaves[i];
        }
    }
    else { // SplitType::HORIZONTAL
        // Horizontal split: firstChild is above, secondChild is below

        // Collect leaf nodes in firstChild and secondChild
        std::vector<LayoutNode*> firstLeaves;
        std::vector<LayoutNode*> secondLeaves;

        std::function<void(LayoutNode*)> collectLeaves = [&](LayoutNode* n) {
            if (!n->isSplit && n->windowInfo.hwnd != nullptr) {
                firstLeaves.push_back(n);
            }
            else {
                if (n->firstChild) collectLeaves(n->firstChild.get());
                if (n->secondChild) collectLeaves(n->secondChild.get());
            }
        };

        collectLeaves(first);
        collectLeaves(second);

        // Map adjacencies: up windows' down to down windows
        size_t count = std::min(firstLeaves.size(), secondLeaves.size());
        for (size_t i = 0; i < count; ++i) {
            firstLeaves[i]->down = secondLeaves[i];
            secondLeaves[i]->up = firstLeaves[i];
        }
    }
}

// Function to apply the layout by traversing the tree
void ApplyLayout(LayoutNode* node, RECT area) {
    if (!node) return;

    if (!node->isSplit) {
        // This is a leaf node; move the window to the specified area
        MoveWindowNormalized(node->windowInfo.hwnd, area.left, area.top,
                             area.right - area.left, area.bottom - area.top);
        node->windowRect = area; // Store the window's position
        return;
    }

    // Calculate the split
    if (node->splitType == SplitType::VERTICAL) {
        int splitPos = area.left + static_cast<int>((area.right - area.left) * node->splitRatio);
        RECT firstArea = { area.left, area.top, splitPos, area.bottom };
        RECT secondArea = { splitPos, area.top, area.right, area.bottom };
        ApplyLayout(node->firstChild.get(), firstArea);
        ApplyLayout(node->secondChild.get(), secondArea);
    }
    else { // SplitType::HORIZONTAL
        int splitPos = area.top + static_cast<int>((area.bottom - area.top) * node->splitRatio);
        RECT firstArea = { area.left, area.top, area.right, splitPos };
        RECT secondArea = { area.left, splitPos, area.right, area.bottom };
        ApplyLayout(node->firstChild.get(), firstArea);
        ApplyLayout(node->secondChild.get(), secondArea);
    }
}

// Function to tile all windows based on the layout tree
void TileWindows(const RECT& screenRect) {
    if (root) {
        ApplyLayout(root.get(), screenRect);
        std::cout << "Windows tiled successfully.\n";
    }
    else {
        std::cerr << "Layout root is null. No windows to tile.\n";
    }
}

// Function to toggle fullscreen for a window
void SetWindowFullscreen(LayoutNode* node, const RECT& monitorRect) {
    if (!node || node->windowInfo.hwnd == nullptr) return;

    WindowInfo& windowInfo = node->windowInfo;

    if (!windowInfo.isFullscreen) {
        // Save current window state
        windowInfo.savedStyle = GetWindowLong(windowInfo.hwnd, GWL_STYLE);
        if (!GetWindowRect(windowInfo.hwnd, &windowInfo.savedRect)) {
            std::cerr << "Failed to get window rect. Error: " << GetLastError() << "\n";
            return;
        }

        // Remove borders, title bar, etc.
        SetWindowLong(windowInfo.hwnd, GWL_STYLE, windowInfo.savedStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE | WS_SYSMENU));

        // Resize and reposition to cover the entire screen
        MoveWindowNormalized(windowInfo.hwnd,
                             monitorRect.left,
                             monitorRect.top,
                             monitorRect.right - monitorRect.left,
                             monitorRect.bottom - monitorRect.top);
    }
    else {
        // Restore original window style
        SetWindowLong(windowInfo.hwnd, GWL_STYLE, windowInfo.savedStyle);
        SetWindowPos(windowInfo.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        // Restore original window size and position
        MoveWindowNormalized(windowInfo.hwnd,
                             windowInfo.savedRect.left,
                             windowInfo.savedRect.top,
                             windowInfo.savedRect.right - windowInfo.savedRect.left,
                             windowInfo.savedRect.bottom - windowInfo.savedRect.top);
    }

    // Toggle the fullscreen flag
    windowInfo.isFullscreen = !windowInfo.isFullscreen;

    // Force redraw
    ShowWindow(windowInfo.hwnd, SW_SHOW);
}

// Function to find a LayoutNode given an HWND
LayoutNode* FindLayoutNode(LayoutNode* node, HWND hwnd) {
    if (!node) return nullptr;
    if (!node->isSplit && node->windowInfo.hwnd == hwnd) return node;
    if (node->isSplit) {
        LayoutNode* found = FindLayoutNode(node->firstChild.get(), hwnd);
        if (found) return found;
        return FindLayoutNode(node->secondChild.get(), hwnd);
    }
    return nullptr;
}

// Function to collect all leaf nodes
void CollectLeafNodes(LayoutNode* node, std::vector<LayoutNode*>& leaves) {
    if (!node) return;
    if (!node->isSplit && node->windowInfo.hwnd != nullptr) {
        leaves.push_back(node);
        return;
    }
    if (node->isSplit) {
        CollectLeafNodes(node->firstChild.get(), leaves);
        CollectLeafNodes(node->secondChild.get(), leaves);
    }
}

// Function to assign cross-split adjacencies based on window positions
void AssignCrossAdjacencies(std::vector<LayoutNode*>& leaves, int tolerance = 5) {
    for (auto& current : leaves) {
        for (auto& other : leaves) {
            if (current == other) continue;

            // Check if 'other' is to the LEFT of 'current'
            if (abs(other->windowRect.right - current->windowRect.left) <= tolerance &&
                (other->windowRect.top < current->windowRect.bottom && other->windowRect.bottom > current->windowRect.top)) {
                current->left = other;
            }

            // Check if 'other' is to the RIGHT of 'current'
            if (abs(other->windowRect.left - current->windowRect.right) <= tolerance &&
                (other->windowRect.top < current->windowRect.bottom && other->windowRect.bottom > current->windowRect.top)) {
                current->right = other;
            }

            // Check if 'other' is ABOVE 'current'
            if (abs(other->windowRect.bottom - current->windowRect.top) <= tolerance &&
                (other->windowRect.left < current->windowRect.right && other->windowRect.right > current->windowRect.left)) {
                current->up = other;
            }

            // Check if 'other' is BELOW 'current'
            if (abs(other->windowRect.top - current->windowRect.bottom) <= tolerance &&
                (other->windowRect.left < current->windowRect.right && other->windowRect.right > current->windowRect.left)) {
                current->down = other;
            }
        }
    }
}

// Function to swap two windows by their LayoutNodes
void SwapWindows(LayoutNode* nodeA, LayoutNode* nodeB) {
    if (!nodeA || !nodeB) return;

    std::swap(nodeA->windowInfo.hwnd, nodeB->windowInfo.hwnd);
    std::swap(nodeA->windowRect, nodeB->windowRect);

    std::cout << "Swapped windows.\n";

    // Re-apply the layout to update window positions
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    TileWindows(screenRect);
}

// Function to focus a window
void FocusWindow(LayoutNode* node) {
    if (!node || node->windowInfo.hwnd == nullptr) return;

    HWND hwnd = node->windowInfo.hwnd;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    std::cout << "Focusing window: " << title << "\n";

    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

// Function to register hotkeys
bool RegisterHotKeys() {
    if (!RegisterHotKey(nullptr, 1, MOD_KEY, VK_LEFT)) return false; // Focus Left
    if (!RegisterHotKey(nullptr, 2, MOD_KEY, VK_RIGHT)) return false; // Focus Right
    if (!RegisterHotKey(nullptr, 3, MOD_KEY, 'F')) return false; // Toggle Fullscreen
    if (!RegisterHotKey(nullptr, 4, MOD_KEY | MOD_SHIFT, VK_LEFT)) return false; // Swap Left
    if (!RegisterHotKey(nullptr, 5, MOD_KEY | MOD_SHIFT, VK_RIGHT)) return false; // Swap Right
    if (!RegisterHotKey(nullptr, 6, MOD_KEY, VK_UP)) return false; // Focus Up
    if (!RegisterHotKey(nullptr, 7, MOD_KEY, VK_DOWN)) return false; // Focus Down
    if (!RegisterHotKey(nullptr, 8, MOD_KEY | MOD_SHIFT, VK_DOWN)) return false; // Swap Down
    if (!RegisterHotKey(nullptr, 9, MOD_KEY | MOD_SHIFT, VK_UP)) return false; // Swap Down
    return true;
}

int main() {
    // Enumerate all visible windows
    std::cout << "Enumerating windows...\n";
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&managedWindows));

    if (managedWindows.empty()) {
        std::cerr << "No windows to manage.\n";
        return 1;
    }

    // Get screen dimensions
    HDC hdcScreen = GetDC(nullptr);
    RECT screenRect;
    screenRect.left = 0;
    screenRect.top = 0;
    screenRect.right = GetDeviceCaps(hdcScreen, HORZRES);
    screenRect.bottom = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(nullptr, hdcScreen);

    std::cout << "Screen dimensions: Width=" << screenRect.right
              << ", Height=" << screenRect.bottom << "\n";

    // Initialize the layout with the first window
    InitializeLayout(managedWindows[0].hwnd);

    // Add remaining windows to the layout
    for (size_t i = 1; i < managedWindows.size(); ++i) {
        AddWindowBreadthFirst(managedWindows[i].hwnd);
    }


    // Establish adjacencies within splits
    EstablishAdjacencies(root.get());

    // Apply the tiling layout and store window positions
    TileWindows(screenRect);

    // Collect all leaf nodes
    std::vector<LayoutNode*> leafNodes;
    CollectLeafNodes(root.get(), leafNodes);

    // Assign cross-split adjacencies based on window positions
    AssignCrossAdjacencies(leafNodes);

    // Register hotkeys for switching and swapping windows
    if (!RegisterHotKeys()) {
        std::cerr << "Failed to register hotkeys.\n";
        return 1;
    }

    std::cout << "Hotkeys registered successfully.\n";
    std::cout << "Available Hotkeys:\n";
    std::cout << "  MOD + LEFT/RIGHT: Focus adjacent windows horizontally.\n";
    std::cout << "  MOD + UP/DOWN: Focus adjacent windows vertically.\n";
    std::cout << "  MOD + SHIFT + LEFT/RIGHT: Swap active window with adjacent horizontally.\n";
    std::cout << "  MOD + F: Toggle fullscreen on the active window.\n";

    // Message loop to handle hotkey events
    MSG msg = { 0 };
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            HWND current = GetForegroundWindow();

            // Find the corresponding LayoutNode
            LayoutNode* currentNode = FindLayoutNode(root.get(), current);
            if (!currentNode) {
                std::cerr << "Current window not managed.\n";
                continue;
            }

            switch (msg.wParam) {
                case 1: { // MOD + LEFT
                    LayoutNode* targetNode = currentNode->left;
                    if (targetNode) {
                        FocusWindow(targetNode);
                    }
                    else {
                        std::cout << "No window to the LEFT.\n";
                    }
                    break;
                }
                case 2: { // MOD + RIGHT
                    LayoutNode* targetNode = currentNode->right;
                    if (targetNode) {
                        FocusWindow(targetNode);
                    }
                    else {
                        std::cout << "No window to the RIGHT.\n";
                    }
                    break;
                }
                case 3: { // MOD + F (Toggle Fullscreen)
                    // Find the corresponding leaf node
                    if (currentNode->windowInfo.hwnd != nullptr) {
                        // Get monitor information for fullscreen
                        HMONITOR hMonitor = MonitorFromWindow(current, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO monitorInfo = { sizeof(monitorInfo) };
                        if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
                            std::cerr << "Failed to get monitor info. Error: " << GetLastError() << "\n";
                            break;
                        }

                        // Toggle fullscreen
                        SetWindowFullscreen(currentNode, monitorInfo.rcMonitor);
                    }
                    else {
                        std::cerr << "Current node does not have a window.\n";
                    }
                    break;
                }
                case 4: { // MOD + SHIFT + LEFT (Swap Left)
                    LayoutNode* targetNode = currentNode->left;
                    if (targetNode) {
                        SwapWindows(currentNode, targetNode);

                        // Re-collect leaf nodes and reassign adjacencies
                        leafNodes.clear();
                        CollectLeafNodes(root.get(), leafNodes);
                        AssignCrossAdjacencies(leafNodes);
                    }
                    else {
                        std::cout << "No window to the LEFT to swap.\n";
                    }
                    break;
                }
                case 5: { // MOD + SHIFT + RIGHT (Swap Right)
                    LayoutNode* targetNode = currentNode->right;
                    if (targetNode) {
                        SwapWindows(currentNode, targetNode);

                        // Re-collect leaf nodes and reassign adjacencies
                        leafNodes.clear();
                        CollectLeafNodes(root.get(), leafNodes);
                        AssignCrossAdjacencies(leafNodes);
                    }
                    else {
                        std::cout << "No window to the RIGHT to swap.\n";
                    }
                    break;
                }
                case 6: { // MOD + UP
                    LayoutNode* targetNode = currentNode->up;
                    if (targetNode) {
                        FocusWindow(targetNode);
                    }
                    else {
                        std::cout << "No window ABOVE.\n";
                    }
                    break;
                }
                case 7: { // MOD + DOWN
                    LayoutNode* targetNode = currentNode->down;
                    if (targetNode) {
                        FocusWindow(targetNode);
                    }
                    else {
                        std::cout << "No window BELOW.\n";
                    }
                    break;
                }
                case 8: { // MOD + SHIFT + DOWN
                    LayoutNode* targetNode = currentNode->down;
                    if (targetNode) {
                        SwapWindows(currentNode, targetNode);

                        // Re-collect leaf nodes and reassign adjacencies
                        leafNodes.clear();
                        CollectLeafNodes(root.get(), leafNodes);
                        AssignCrossAdjacencies(leafNodes);
                    }
                    else {
                        std::cout << "No window BELOW.\n";
                    }
                    break;
                }
                case 9: { // MOD + SHIFT + UP
                    LayoutNode* targetNode = currentNode->up;
                    if (targetNode) {
                        SwapWindows(currentNode, targetNode);

                        // Re-collect leaf nodes and reassign adjacencies
                        leafNodes.clear();
                        CollectLeafNodes(root.get(), leafNodes);
                        AssignCrossAdjacencies(leafNodes);
                    }
                    else {
                        std::cout << "No window UP to swap current focused window with.\n";
                    }
                    break;
                }
                default:
                    break;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
