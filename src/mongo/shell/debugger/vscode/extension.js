/**
 * VSCode Extension for MongoDB Shell Debugger
 *
 * Registers the 'mongo-shell' debug type and provides configuration.
 */

const vscode = require("vscode");
const path = require("path");

// Extension entry point
function activate(context) {
    checkIfNewerVersionAvailable(context);

    // Register debug config provider
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider("mongo-shell", {
            provideDebugConfigurations,
        }),
    );

    // Register debug adapter factory
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory("mongo-shell", {
            createDebugAdapterDescriptor: (session) => createDebugAdapter(context, session),
        }),
    );
}

function deactivate() {}

function provideDebugConfigurations(_folder) {
    return [
        {
            type: "mongo-shell",
            request: "attach",
            name: "Attach to MongoDB Shell",
            debugPort: 9229,
        },
    ];
}

function createDebugAdapter(context, _session) {
    const adapterPath = path.join(context.extensionPath, "adapter.js");
    return new vscode.DebugAdapterExecutable("node", [adapterPath]);
}

function checkIfNewerVersionAvailable(context) {
    const extensionSrcPath = "src/mongo/shell/debugger/vscode";

    try {
        const mongoRepo = findMongoRepo();
        if (!mongoRepo) {
            return;
        }

        const fs = require("fs");
        const gitTreePackageJsonPath = path.join(mongoRepo, extensionSrcPath, "package.json");

        if (!fs.existsSync(gitTreePackageJsonPath)) {
            return;
        }

        const installedVersion = context.extension.packageJSON.version;
        const gitTreePackageJson = JSON.parse(fs.readFileSync(gitTreePackageJsonPath, "utf8"));
        const gitTreeVersion = gitTreePackageJson.version;

        if (installedVersion !== gitTreeVersion) {
            const installScriptPath = path.join(extensionSrcPath, "install.sh");

            vscode.window
                .showWarningMessage(
                    `MongoDB Shell Debugger extension update available (installed: ${installedVersion}, available: ${gitTreeVersion}). Run the install.sh to update.`,
                    "Copy Install Command",
                )
                .then((selection) => {
                    if (selection === "Copy Install Command") {
                        vscode.env.clipboard.writeText(installScriptPath).then(() => {
                            vscode.window.showInformationMessage("Install command copied to clipboard!");
                        });
                    }
                });
        }
    } catch (error) {
        // Silently fail - don't disrupt extension activation
        console.error("Version check failed:", error);
    }
}

function findMongoRepo() {
    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (workspaceFolders?.length > 0) {
        // Check if any workspace folder looks like the mongo repo
        for (const folder of workspaceFolders) {
            const repoPath = folder.uri.fsPath;
            // Verify it's the mongo repo by checking for a marker file
            const fs = require("fs");
            const markerPath = path.join(repoPath, "src/mongo/shell/debugger/vscode/package.json");
            if (fs.existsSync(markerPath)) {
                return repoPath;
            }
        }
        // No workspace folders had the marker file, so just take the first one to hopefully be more relevant than nothing (null).
        return workspaceFolders[0].uri.fsPath;
    }
    return null;
}

module.exports = {activate, deactivate};
