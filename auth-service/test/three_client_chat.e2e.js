'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {existsSync} = require('node:fs');
const {mkdir} = require('node:fs/promises');
const net = require('node:net');
const path = require('node:path');

const {createRedisClient, connectRedis, closeRedis} = require('../src/redis_client');
const {requestJson} = require('./http_test_helpers');
const {withDeadline} = require('./e2e_deadline');
const {createE2eRunContext} = require('./e2e_run_context');

const REDIS_URL = process.env.CHATHUB_REDIS_TEST_URL;
const AUTH_SERVICE_PORT = 3000;
const FRAME_MAGIC = 0x4348;
const PROTOCOL_VERSION = 1;
const CHAT_FRAME_TYPE = 1;
const AUTH_FRAME_TYPE = 5;
const AUTH_RESPONSE_FRAME_TYPE = 5;
const CHAT_ACK_FRAME_TYPE = 6;
const DELIVERY_RECEIPT_FRAME_TYPE = 7;
const ONLINE_USERS_FRAME_TYPE = 8;
const MAX_FRAME_BODY_LENGTH = 2048;
const STARTUP_TIMEOUT_MS = 10000;
const OPERATION_TIMEOUT_MS = 5000;
const PROCESS_EXIT_TIMEOUT_MS = 5000;
const OBSERVER_SILENCE_TIMEOUT_MS = 150;

if (typeof REDIS_URL !== 'string' || REDIS_URL.trim().length === 0) {
    throw new Error('CHATHUB_REDIS_TEST_URL_missing');
}

function delay(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

// 连接探针只判断端口是否可访问，不能替代 Auth、ChatServer 的业务验收。
function probeTcpPort(port, timeout_ms = 1000) {
    return new Promise(resolve => {
        const socket = net.createConnection({host: '127.0.0.1', port});
        let settled = false;
        const timeout_id = setTimeout(() => finish('TIMEOUT'), timeout_ms);

        const finish = result => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeout_id);
            socket.destroy();
            resolve(result);
        };

        socket.once('connect', () => finish('OPEN'));
        socket.once('error', error => finish(error.code || 'ERROR'));
    });
}

function acquireUnusedPort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('unused_port_invalid')));
                return;
            }
            server.close(error => error ? reject(error) : resolve(address.port));
        });
    });
}

function startProcess(executable, arguments_list, {cwd, env}) {
    const child = spawn(executable, arguments_list, {
        cwd,
        env,
        stdio: ['ignore', 'pipe', 'pipe']
    });
    let output = '';
    let spawn_error;

    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { output += chunk; });
    child.once('error', error => { spawn_error = error; });

    return {
        child,
        output: () => output,
        spawn_error: () => spawn_error,
        exit: new Promise(resolve => {
            child.once('exit', (code, signal) => resolve({code, signal}));
        })
    };
}

// 启动日志是进程已进入可服务状态的证据；超时或提前退出都返回稳定错误。
async function waitForProcessOutput(process_handle, marker, label) {
    const deadline = Date.now() + STARTUP_TIMEOUT_MS;
    while (!process_handle.output().includes(marker)) {
        if (process_handle.child.exitCode !== null) {
            const spawn_error = process_handle.spawn_error();
            throw new Error(
                `${label}_process_exited:${process_handle.child.exitCode ?? 'null'}:`
                + `${spawn_error?.code || 'no_spawn_error'}`
            );
        }
        if (Date.now() >= deadline) {
            throw new Error(`${label}_timeout`);
        }
        await delay(25);
    }
}

async function waitForPortClosed(port, label) {
    const deadline = Date.now() + PROCESS_EXIT_TIMEOUT_MS;
    while (await probeTcpPort(port) === 'OPEN') {
        if (Date.now() >= deadline) {
            throw new Error(`${label}_timeout`);
        }
        await delay(25);
    }
}

async function stopProcess(process_handle, label) {
    if (!process_handle || process_handle.child.exitCode !== null) {
        return;
    }
    process_handle.child.kill();
    await withDeadline(process_handle.exit, PROCESS_EXIT_TIMEOUT_MS, label);
}

function makeFrame(type, body) {
    const body_buffer = Buffer.from(body, 'utf8');
    assert.ok(body_buffer.length <= MAX_FRAME_BODY_LENGTH);

    const frame = Buffer.alloc(8 + body_buffer.length);
    frame.writeUInt16BE(FRAME_MAGIC, 0);
    frame.writeUInt8(PROTOCOL_VERSION, 2);
    frame.writeUInt8(type, 3);
    frame.writeUInt32BE(body_buffer.length, 4);
    body_buffer.copy(frame, 8);
    return frame;
}

function createFrameClient(socket) {
    let received = Buffer.alloc(0);
    let pending_read = null;
    let pending_timeout_id = null;
    let socket_error = null;

    const rejectPendingRead = error => {
        if (!pending_read) {
            return;
        }
        const current = pending_read;
        pending_read = null;
        clearTimeout(pending_timeout_id);
        pending_timeout_id = null;
        current.reject(error);
    };

    const readOneFrame = () => {
        if (received.length < 8) {
            return null;
        }
        const magic = received.readUInt16BE(0);
        const version = received.readUInt8(2);
        const type = received.readUInt8(3);
        const body_length = received.readUInt32BE(4);
        if (magic !== FRAME_MAGIC || version !== PROTOCOL_VERSION
            || body_length > MAX_FRAME_BODY_LENGTH) {
            throw new Error('invalid_chat_frame');
        }
        const frame_length = 8 + body_length;
        if (received.length < frame_length) {
            return null;
        }
        const body = received.subarray(8, frame_length).toString('utf8');
        received = received.subarray(frame_length);
        return {type, body};
    };

    const tryResolveRead = () => {
        if (!pending_read) {
            return;
        }
        let frame;
        try {
            frame = readOneFrame();
        } catch (error) {
            rejectPendingRead(error);
            return;
        }
        if (!frame) {
            return;
        }
        const current = pending_read;
        pending_read = null;
        clearTimeout(pending_timeout_id);
        pending_timeout_id = null;
        current.resolve(frame);
    };

    socket.on('data', chunk => {
        received = Buffer.concat([received, chunk]);
        tryResolveRead();
    });
    socket.once('error', error => {
        socket_error = error;
        rejectPendingRead(error);
    });
    socket.once('close', () => {
        rejectPendingRead(socket_error || new Error('tcp_socket_closed'));
    });

    return {
        writeFrame(type, body) {
            if (socket.destroyed) {
                throw new Error('tcp_socket_destroyed');
            }
            socket.write(makeFrame(type, body));
        },
        readFrame(timeout_ms = OPERATION_TIMEOUT_MS) {
            if (pending_read) {
                return Promise.reject(new Error('concurrent_frame_read'));
            }
            return new Promise((resolve, reject) => {
                pending_read = {resolve, reject};
                pending_timeout_id = setTimeout(() => {
                    rejectPendingRead(new Error('tcp_frame_read_timeout'));
                }, timeout_ms);
                tryResolveRead();
            });
        },
        destroy() {
            socket.destroy();
        }
    };
}

function connectFrameClient(port) {
    return new Promise((resolve, reject) => {
        const socket = net.createConnection({host: '127.0.0.1', port});
        let settled = false;
        const timeout_id = setTimeout(
            () => finish(null, new Error('tcp_connect_timeout')),
            OPERATION_TIMEOUT_MS
        );

        const finish = (value, error) => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeout_id);
            socket.removeListener('error', on_error);
            if (error) {
                socket.destroy();
                reject(error);
                return;
            }
            resolve(createFrameClient(value));
        };
        const on_error = error => finish(null, error);

        socket.once('connect', () => finish(socket, null));
        socket.once('error', on_error);
    });
}

async function readJsonFrame(frame_client, expected_type, label) {
    const frame = await withDeadline(
        frame_client.readFrame(), OPERATION_TIMEOUT_MS, label
    );
    assert.equal(frame.type, expected_type, `${label}_unexpected_type`);
    return JSON.parse(frame.body);
}

async function expectOnlineUsers(frame_client, expected_users, label) {
    const body = await readJsonFrame(frame_client, ONLINE_USERS_FRAME_TYPE, label);
    assert.deepEqual(body.users, [...expected_users].sort());
}

async function authenticate(frame_client, token, expected_users, label) {
    frame_client.writeFrame(AUTH_FRAME_TYPE, token);
    const auth_body = await readJsonFrame(
        frame_client, AUTH_RESPONSE_FRAME_TYPE, `${label}_auth_response`
    );
    assert.equal(auth_body.ok, true);
    await expectOnlineUsers(frame_client, expected_users, `${label}_online_users`);
}

async function requestJsonWithDeadline(base_url, route, options, label) {
    return withDeadline(requestJson(base_url, route, options), OPERATION_TIMEOUT_MS, label);
}

async function registerAndLogin(base_url, username, password, label) {
    const registration = await requestJsonWithDeadline(base_url, '/register', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    }, `${label}_register`);
    assert.equal(registration.status, 201);

    const login = await requestJsonWithDeadline(base_url, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    }, `${label}_login`);
    assert.equal(login.status, 200);
    assert.equal(login.body.username, username);
    assert.equal(typeof login.body.token, 'string');
    return login.body.token;
}

// Redis 6 的 scanIterator 每轮可能产出一批 key 数组；统一展平后才允许删除或断言。
async function collectRunRedisKeys(redis_client, redis_key_prefix) {
    const keys = [];
    for await (const scan_result of redis_client.scanIterator({
        MATCH: `${redis_key_prefix}*`,
        COUNT: 100
    })) {
        if (Array.isArray(scan_result)) {
            keys.push(...scan_result);
        } else {
            keys.push(scan_result);
        }
    }
    return keys;
}

async function deleteRunRedisKeys(redis_client, redis_key_prefix) {
    const keys = await collectRunRedisKeys(redis_client, redis_key_prefix);
    if (keys.length > 0) {
        await redis_client.del(keys);
    }
}

async function relayAndConfirm({sender, recipient, observer, run_id, sequence, statistics}) {
    const local_id = `w13_${run_id.slice(0, 12)}_${sequence}`;
    const content = `controlled-load-${sequence}`;
    const send_at = new Date().toISOString();

    sender.client.writeFrame(CHAT_FRAME_TYPE, JSON.stringify({
        to: recipient.username,
        local_id,
        content,
        send_at
    }));
    statistics.sent += 1;

    const forwarded = await readJsonFrame(
        recipient.client, CHAT_FRAME_TYPE, `${sequence}_forwarded`
    );
    const acknowledgement = await readJsonFrame(
        sender.client, CHAT_ACK_FRAME_TYPE, `${sequence}_acknowledgement`
    );

    assert.equal(acknowledgement.local_id, local_id);
    assert.equal(acknowledgement.status, 'accepted');
    assert.equal(typeof acknowledgement.message_id, 'string');
    assert.ok(acknowledgement.message_id.length > 0);
    assert.ok(Number.isSafeInteger(acknowledgement.server_received_at_ms));
    statistics.acknowledged += 1;

    assert.equal(forwarded.message_id, acknowledgement.message_id);
    assert.equal(forwarded.local_id, local_id);
    assert.equal(forwarded.from, sender.username);
    assert.equal(forwarded.to, recipient.username);
    assert.equal(forwarded.content, content);
    assert.equal(forwarded.send_at, send_at);
    assert.equal(forwarded.server_received_at_ms, acknowledgement.server_received_at_ms);
    statistics.forwarded += 1;

    // 观察者在短 deadline 内只允许得到“没有消息”的稳定超时，不能收到他人的 chat。
    await assert.rejects(
        () => observer.client.readFrame(OBSERVER_SILENCE_TIMEOUT_MS),
        error => error instanceof Error && error.message === 'tcp_frame_read_timeout'
    );

    recipient.client.writeFrame(DELIVERY_RECEIPT_FRAME_TYPE, JSON.stringify({
        message_id: acknowledgement.message_id
    }));
    const delivered = await readJsonFrame(
        sender.client, DELIVERY_RECEIPT_FRAME_TYPE, `${sequence}_delivered`
    );
    assert.equal(delivered.local_id, local_id);
    assert.equal(delivered.status, 'delivered');
    statistics.delivered += 1;
}

test('真实 Auth 与 ChatServer 下三个客户端只向目标用户转发消息', {timeout: 60000}, async t => {
    const chat_server_executable = process.env.CHATHUB_CHAT_SERVER_EXECUTABLE
        ? path.resolve(process.env.CHATHUB_CHAT_SERVER_EXECUTABLE)
        : path.resolve(__dirname, '..', '..', 'cmake-build-debug-mysql',
            'chat-server', 'chat-server.exe');
    assert.ok(existsSync(chat_server_executable), 'chat_server_executable_missing');
    assert.equal(
        await probeTcpPort(AUTH_SERVICE_PORT),
        'ECONNREFUSED',
        'auth_service_port_3000_must_be_free'
    );

    const context = await createE2eRunContext();
    const auth_directory = path.join(context.temporary_directory, 'auth');
    const chat_directory = path.join(context.temporary_directory, 'chat');
    const chat_database_path = path.join(chat_directory, 'messages.db');
    const chat_server_port = await acquireUnusedPort();
    const run_id = context.run_id.replace(/-/g, '');
    const auth_redis_key_prefix = context.redis_key_prefix.slice(0, -1);
    const base_url = `http://127.0.0.1:${AUTH_SERVICE_PORT}`;
    const password = 'password123';
    const secret_key = `w13-test-secret-${run_id}`;
    const internal_service_key = `w13-test-internal-${run_id}`;
    const redis_client = createRedisClient({
        url: REDIS_URL.trim(),
        connectTimeoutMs: 2000,
        maxReconnectAttempts: 0,
        reconnectDelayMs: 0
    });

    t.after(async () => {
        let cleanup_error;
        try {
            await context.cleanup();
        } catch (error) {
            cleanup_error = error;
        }

        // 使用新连接核对本次 prefix 已清空；不读取、不删除其他测试的 key。
        const verifier = createRedisClient({
            url: REDIS_URL.trim(),
            connectTimeoutMs: 2000,
            maxReconnectAttempts: 0,
            reconnectDelayMs: 0
        });
        try {
            await withDeadline(connectRedis(verifier), OPERATION_TIMEOUT_MS, 'redis_cleanup_verify_connect');
            const remaining_keys = await collectRunRedisKeys(
                verifier,
                context.redis_key_prefix
            );
            assert.deepEqual(remaining_keys, []);
        } finally {
            await closeRedis(verifier);
        }

        if (cleanup_error) {
            throw cleanup_error;
        }
    });

    await mkdir(auth_directory, {recursive: true});
    await mkdir(chat_directory, {recursive: true});
    context.defer(async () => {
        try {
            if (redis_client.isOpen && redis_client.isReady) {
                await deleteRunRedisKeys(redis_client, context.redis_key_prefix);
            }
        } finally {
            await closeRedis(redis_client);
        }
    });
    await withDeadline(connectRedis(redis_client), OPERATION_TIMEOUT_MS, 'redis_connect');

    const auth_process = startProcess(
        process.execPath,
        [path.resolve(__dirname, '..', 'src', 'server.js')],
        {
            cwd: auth_directory,
            env: {
                ...process.env,
                CHATHUB_REDIS_URL: REDIS_URL.trim(),
                SECRET_KEY: secret_key,
                CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key,
                CHATHUB_REDIS_KEY_PREFIX: auth_redis_key_prefix,
                CHATHUB_LOGIN_USER_LIMIT: '5',
                CHATHUB_LOGIN_IP_LIMIT: '20',
                CHATHUB_LOGIN_WINDOW_SECONDS: '60'
            }
        }
    );
    context.defer(async () => {
        try {
            await stopProcess(auth_process, 'auth_process_exit');
        } finally {
            await waitForPortClosed(AUTH_SERVICE_PORT, 'auth_port_close');
        }
    });
    await waitForProcessOutput(
        auth_process,
        'component=auth phase=startup event=listening port=3000',
        'auth_startup'
    );

    const users = await Promise.all(['alice', 'bob', 'carol'].map(async role => {
        const username = `w13_${run_id.slice(0, 8)}_${role}`;
        const token = await registerAndLogin(base_url, username, password, role);
        return {role, username, token};
    }));
    const [alice, bob, carol] = users;

    const chat_process = startProcess(
        chat_server_executable,
        [
            '--port', String(chat_server_port),
            '--database-path', chat_database_path,
            '--auth-timeout-ms', '5000'
        ],
        {
            cwd: chat_directory,
            env: {
                ...process.env,
                CHATHUB_AUTH_INTROSPECTION_URL:
                    `${base_url}/internal/auth/introspect`,
                CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key
            }
        }
    );
    context.defer(async () => {
        try {
            await stopProcess(chat_process, 'chat_process_exit');
        } finally {
            await waitForPortClosed(chat_server_port, 'chat_port_close');
        }
    });
    await waitForProcessOutput(chat_process, 'server_started', 'chat_startup');
    assert.equal(await probeTcpPort(chat_server_port), 'OPEN');

    for (const user of users) {
        user.client = await withDeadline(
            connectFrameClient(chat_server_port),
            OPERATION_TIMEOUT_MS,
            `${user.role}_connect`
        );
        context.defer(async () => user.client.destroy());
    }

    await authenticate(alice.client, alice.token, [alice.username], 'alice');
    await authenticate(bob.client, bob.token, [alice.username, bob.username], 'bob');
    await expectOnlineUsers(alice.client, [alice.username, bob.username], 'alice_after_bob');
    await authenticate(
        carol.client,
        carol.token,
        [alice.username, bob.username, carol.username],
        'carol'
    );
    await expectOnlineUsers(
        alice.client,
        [alice.username, bob.username, carol.username],
        'alice_after_carol'
    );
    await expectOnlineUsers(
        bob.client,
        [alice.username, bob.username, carol.username],
        'bob_after_carol'
    );

    const statistics = {sent: 0, acknowledged: 0, forwarded: 0, delivered: 0};
    await relayAndConfirm({
        sender: alice, recipient: bob, observer: carol,
        run_id, sequence: 'alice_to_bob', statistics
    });
    await relayAndConfirm({
        sender: bob, recipient: carol, observer: alice,
        run_id, sequence: 'bob_to_carol', statistics
    });
    await relayAndConfirm({
        sender: carol, recipient: alice, observer: bob,
        run_id, sequence: 'carol_to_alice', statistics
    });

    assert.deepEqual(statistics, {sent: 3, acknowledged: 3, forwarded: 3, delivered: 3});
    t.diagnostic('scenario=three_client_real_auth sent=3 acknowledged=3 forwarded=3 delivered=3');
});
