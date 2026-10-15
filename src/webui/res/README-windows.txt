Chains
======

Chains is a Bitcoin Core based chain with drivechains (BIP300-style) and blind
merged mining. chains-app.exe is its node, its wallet and its interface in one
program; the interface is shown by Microsoft Edge WebView2, which Windows 10 and
11 have. Without it, install the WebView2 Runtime (Evergreen) from
https://developer.microsoft.com/microsoft-edge/webview2/

Setup
-----
Unpack the files into a folder and run chains-app.exe (or use the installer).
The node and its wallets keep their data in %APPDATA%\Chains, as chains-qt does;
the settings of the app are in %APPDATA%\Chains\chains-app.json.

The programs without an interface (chainsd, chains-cli, chains-tx, chains-wallet,
chains-util) are in the daemon folder.

License
-------
Distributed under the MIT software license, see COPYING.txt.
