'use strict';

const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');

let client;
let output;

function getServerCommand() {
    return vscode.workspace
        .getConfiguration('lifeful')
        .get('serverPath', 'lifeful') || 'lifeful';
}

function createClient() {
    const command = getServerCommand();

    const serverOptions = {
        command,
        args: ['--stdio'],
        options: { shell: process.platform === 'win32' }
    };

    const clientOptions = {
        documentSelector: [
            { scheme: 'file', language: 'lifeless' },
            { scheme: 'untitled', language: 'lifeless' }
        ],
        outputChannel: output
    };

    output.appendLine(`Starting: ${command} ${serverOptions.args.join(' ')}`);

    return new LanguageClient(
        'lifeful',
        'Lifeful Language Server',
        serverOptions,
        clientOptions
    );
}

async function startClient() {
    client = createClient();
    try {
        await client.start();
    } catch (error) {
        output.appendLine(`[start error] ${error.message}`);
        client = undefined;

        const choice = await vscode.window.showErrorMessage(
            `Lifeful: failed to start language server "${getServerCommand()}". ` +
            'Make sure it is installed and on PATH, or set "lifeful.serverPath".',
            'Open Settings'
        );
        if (choice === 'Open Settings') {
            vscode.commands.executeCommand(
                'workbench.action.openSettings',
                'lifeful.serverPath'
            );
        }
    }
}

async function stopClient() {
    if (!client) return;
    const old = client;
    client = undefined;
    try {
        await old.stop();
    } catch (error) {
        output.appendLine(`[stop error] ${error.message}`);
    }
}

async function restartClient() {
    await stopClient();
    await startClient();
}

async function activate(context) {
    output = vscode.window.createOutputChannel('Lifeful');
    context.subscriptions.push(output);

    context.subscriptions.push(
        vscode.commands.registerCommand('lifeful.restartServer', restartClient),
        vscode.workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration('lifeful.serverPath')) {
                restartClient();
            }
        })
    );

    await startClient();
}

async function deactivate() {
    await stopClient();
}

module.exports = { activate, deactivate };