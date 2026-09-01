'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const net = require('node:net');
const path = require('node:path');

const projectRoot = path.resolve(__dirname, '..');
const authHttpPort = 3000;

// 功能：让操作系统分配一个临时端口，再释放它作为“确定未监听”的 Redis 目标。
function acquireUnusedPort() {
    return new Promise((resolve, reject) => {
        const probeServer = net.createServer();
        probeServer.once('error', reject);
        probeServer.listen(0, '127.0.0.1', () => {
            const address = probeServer.address();
            if (!address || typeof address === 'string') {
                probeServer.close(() => reject(new Error('probe_port_invalid')));
                return;
            }

            probeServer.close(error => {
                if (error) {
                    reject(error);
                    return;
                }
                resolve(address.port);
            });
        });
    });
}

// 功能：区分端口拒绝、已监听和连接超时，避免只用布尔值掩盖启动状态。
function probeTcpPort(port, {timeoutMs = 1000} = {}) {
    return new Promise(resolve => {
        const socket = net.createConnection({
            host: '127.0.0.1',
            port
        });
        let settled = false;
        const finish = result => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeoutId);
            socket.destroy();
            resolve(result);
        };
        const timeoutId = setTimeout(() => finish('TIMEOUT'), timeoutMs);

        socket.once('connect', () => finish('OPEN'));
        socket.once('error', error => finish(error.code || 'ERROR'));
    });
}

// 功能：等待真实 server.js 进程退出，并收集 stdout/stderr 供断言使用。
function waitForChildExit(child, {timeoutMs = 7000} = {}) {
    return new Promise((resolve, reject) => {
        let stdout = '';
        let stderr = '';
        let settled = false;
        const timeoutId = setTimeout(() => {
            if (settled) {
                return;
            }
            settled = true;
            child.kill();
            reject(new Error('auth_startup_exit_timeout'));
        }, timeoutMs);

        child.stdout.setEncoding('utf8');
        child.stderr.setEncoding('utf8');
        child.stdout.on('data', chunk => {
            stdout += chunk;
        });
        child.stderr.on('data', chunk => {
            stderr += chunk;
        });
        child.once('error', error => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeoutId);
            reject(error);
        });
        child.once('exit', (code, signal) => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeoutId);
            resolve({code, signal, stdout, stderr});
        });
    });
}

test('当 Redis 启动依赖失败时，Auth Service 应退出且不监听 HTTP 端口', {timeout: 15000}, async () => {
    assert.equal(
        await probeTcpPort(authHttpPort),
        'ECONNREFUSED',
        '测试开始前 3000 端口必须空闲'
    );

    const redisPort = await acquireUnusedPort();
    const child = spawn(process.execPath, ['src/server.js'], {
        cwd: projectRoot,
        env: {
            ...process.env,
            CHATHUB_REDIS_URL: `redis://127.0.0.1:${redisPort}`,
            SECRET_KEY: `startup-${Date.now()}`,
            CHATHUB_AUTH_INTERNAL_SERVICE_KEY: `startup-internal-${process.pid}`,
            CHATHUB_REDIS_KEY_PREFIX: `chathub:test:startup:${process.pid}`,
            CHATHUB_LOGIN_USER_LIMIT: '5',
            CHATHUB_LOGIN_IP_LIMIT: '20',
            CHATHUB_LOGIN_WINDOW_SECONDS: '60'
        },
        stdio: ['ignore', 'pipe', 'pipe']
    });

    const result = await waitForChildExit(child);
    assert.notEqual(result.code, 0);
    assert.equal(result.signal, null);
    assert.match(
        `${result.stdout}\n${result.stderr}`,
        /code=(?:ECONNREFUSED|auth_startup_failed)/
    );
    assert.doesNotMatch(result.stdout, /event=listening/);
    assert.doesNotMatch(result.stderr, /event=listening/);
    assert.equal(await probeTcpPort(authHttpPort), 'ECONNREFUSED');
});
