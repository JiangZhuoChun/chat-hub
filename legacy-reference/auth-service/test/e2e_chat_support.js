'use strict';

// W13 的真实 TCP 场景共用这层测试支持代码：它只编排进程和协议帧，
// 不伪造 Auth、Redis 或 ChatServer 的任何业务结果。
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

const AUTH_SERVICE_PORT = 3000;
const FRAME_MAGIC = 0x4348;
const PROTOCOL_VERSION = 1;
const CHAT_FRAME_TYPE = 1;
const ERROR_FRAME_TYPE = 4;
const AUTH_FRAME_TYPE = 5;
const AUTH_RESPONSE_FRAME_TYPE = 5;
const CHAT_ACK_FRAME_TYPE = 6;
const DELIVERY_RECEIPT_FRAME_TYPE = 7;
const ONLINE_USERS_FRAME_TYPE = 8;
const MAX_FRAME_BODY_LENGTH = 2048;
const STARTUP_TIMEOUT_MS = 10000;
const OPERATION_TIMEOUT_MS = 5000;
const PROCESS_EXIT_TIMEOUT_MS = 5000;

function redisTestUrl() {
    const url = process.env.CHATHUB_REDIS_TEST_URL;
    if (typeof url !== 'string' || url.trim().length === 0) {
        throw new Error('CHATHUB_REDIS_TEST_URL_missing');
    }
    return url.trim();
}

function delay(milliseconds) {
    return new Promise(resolve => setTimeout(resolve, milliseconds));
}

// 连接探针只判断端口是否已开放，不能当作 Auth 或 ChatServer 的业务成功证据。
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

// 只有服务自身的 listening/server_started 日志出现，才进入下一步 TCP 或 HTTP 业务测试。
async function waitForProcessOutput(process_handle, marker, label) {
    const deadline = Date.now() + STARTUP_TIMEOUT_MS;
    while (!process_handle.output().includes(marker)) {
        const spawn_error = process_handle.spawn_error();
        if (spawn_error) {
            throw new Error(`${label}_spawn_failed:${spawn_error.code || 'unknown'}`);
        }
        if (process_handle.child.exitCode !== null) {
            throw new Error(`${label}_process_exited:${process_handle.child.exitCode}`);
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
    assert.ok(body_buffer.length <= MAX_FRAME_BODY_LENGTH, 'chat_frame_body_too_large');

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
    let resolve_closed;
    const closed = new Promise(resolve => { resolve_closed = resolve; });

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
        resolve_closed();
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
        close() {
            socket.destroy();
            return closed;
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

async function closeFrameClient(frame_client, label) {
    if (!frame_client) {
        return;
    }
    await withDeadline(frame_client.close(), PROCESS_EXIT_TIMEOUT_MS, label);
}

async function readJsonFrame(frame_client, expected_type, label) {
    const frame = await withDeadline(frame_client.readFrame(), OPERATION_TIMEOUT_MS, label);
    assert.equal(frame.type, expected_type, `${label}_unexpected_type`);
    return JSON.parse(frame.body);
}

async function expectOnlineUsers(frame_client, expected_users, label) {
    const body = await readJsonFrame(frame_client, ONLINE_USERS_FRAME_TYPE, label);
    assert.deepEqual(body.users, [...expected_users].sort(), `${label}_unexpected_users`);
}

async function authenticate(frame_client, token, expected_users, label) {
    frame_client.writeFrame(AUTH_FRAME_TYPE, token);
    const auth_body = await readJsonFrame(
        frame_client, AUTH_RESPONSE_FRAME_TYPE, `${label}_auth_response`
    );
    assert.equal(auth_body.ok, true, `${label}_auth_not_ok`);
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
    assert.equal(registration.status, 201, `${label}_register_status`);

    const login = await requestJsonWithDeadline(base_url, '/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username, password})
    }, `${label}_login`);
    assert.equal(login.status, 200, `${label}_login_status`);
    assert.equal(login.body.username, username, `${label}_login_username`);
    assert.equal(typeof login.body.token, 'string', `${label}_login_token`);
    return login.body.token;
}

// Redis 6 的 scanIterator 每轮可能产出一批 key 数组；先展平才能安全删除或断言无残留。
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

function resolveChatServerExecutable() {
    const executable = process.env.CHATHUB_CHAT_SERVER_EXECUTABLE
        ? path.resolve(process.env.CHATHUB_CHAT_SERVER_EXECUTABLE)
        : path.resolve(__dirname, '..', '..', 'cmake-build-debug-mysql',
            'chat-server', 'chat-server.exe');
    assert.ok(existsSync(executable), 'chat_server_executable_missing');
    return executable;
}

// 创建真实 Auth、Redis、ChatServer 的隔离运行环境。每项资源都在 context 中登记，
// 因而异常也会按客户端 -> ChatServer -> Auth -> Redis -> 临时目录的反序回收。
async function createChatEnvironment(t, options = {}) {
    const redis_url = redisTestUrl();
    const chat_server_executable = resolveChatServerExecutable();
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
    const base_url = `http://127.0.0.1:${AUTH_SERVICE_PORT}`;
    const secret_key = `w13-test-secret-${run_id}`;
    const internal_service_key = `w13-test-internal-${run_id}`;
    const redis_client = createRedisClient({
        url: redis_url,
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

        // 使用新的 Redis 连接只检查本次 prefix；不读写其他运行留下的 key。
        const verifier = createRedisClient({
            url: redis_url,
            connectTimeoutMs: 2000,
            maxReconnectAttempts: 0,
            reconnectDelayMs: 0
        });
        try {
            await withDeadline(connectRedis(verifier), OPERATION_TIMEOUT_MS, 'redis_cleanup_verify_connect');
            const remaining_keys = await collectRunRedisKeys(verifier, context.redis_key_prefix);
            assert.deepEqual(remaining_keys, [], 'run_redis_keys_not_cleaned');
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
                CHATHUB_REDIS_URL: redis_url,
                SECRET_KEY: secret_key,
                CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key,
                CHATHUB_REDIS_KEY_PREFIX: context.redis_key_prefix.slice(0, -1),
                CHATHUB_LOGIN_USER_LIMIT: '5',
                // 20 客户端场景恰好需要 20 次真实 /login，不能因测试工具自身被限流。
                CHATHUB_LOGIN_IP_LIMIT: '20',
                CHATHUB_LOGIN_WINDOW_SECONDS: '60',
                ...(options.auth_environment || {})
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

    const environment = {
        context,
        run_id,
        auth_directory,
        chat_directory,
        chat_database_path,
        chat_server_port,
        base_url,
        secret_key,
        internal_service_key,
        auth_process,
        redis_client
    };

    const chat_configuration = options.prepare_chat_server
        ? await options.prepare_chat_server(environment)
        : {arguments: [
            '--port', String(chat_server_port),
            '--database-path', chat_database_path,
            '--auth-timeout-ms', '5000'
        ]};
    assert.ok(chat_configuration && Array.isArray(chat_configuration.arguments),
        'chat_server_arguments_missing');

    const chat_process = startProcess(chat_server_executable, chat_configuration.arguments, {
        cwd: chat_directory,
        env: {
            ...process.env,
            CHATHUB_AUTH_INTROSPECTION_URL: `${base_url}/internal/auth/introspect`,
            CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key,
            ...(chat_configuration.environment || {})
        }
    });
    context.defer(async () => {
        try {
            await stopProcess(chat_process, 'chat_process_exit');
        } finally {
            await waitForPortClosed(chat_server_port, 'chat_port_close');
        }
    });
    await waitForProcessOutput(chat_process, 'server_started', 'chat_startup');
    assert.equal(await probeTcpPort(chat_server_port), 'OPEN', 'chat_server_port_not_open');

    environment.chat_process = chat_process;
    return environment;
}

async function createUsers(environment, count, role_prefix = 'u') {
    assert.ok(Number.isInteger(count) && count > 0, 'client_count_invalid');
    const users = [];
    for (let index = 1; index <= count; index += 1) {
        const role = `${role_prefix}${String(index).padStart(2, '0')}`;
        // 用户名上限为 20；run_id 只取 8 位，给客户端序号保留稳定空间。
        const username = `w13_${environment.run_id.slice(0, 8)}_${role}`;
        const token = await registerAndLogin(
            environment.base_url, username, 'password123', role
        );
        users.push({role, username, token, client: null});
    }
    return users;
}

// 顺序认证时必须消费旧用户收到的 online_users 更新，才能保证下一步读到的是业务帧而不是遗留快照。
async function connectAndAuthenticateUsers(environment, users, statistics) {
    const authenticated = [];
    for (const user of users) {
        user.client = await withDeadline(
            connectFrameClient(environment.chat_server_port), OPERATION_TIMEOUT_MS,
            `${user.role}_connect`
        );
        environment.context.defer(async () => closeFrameClient(user.client, `${user.role}_cleanup_close`));
        statistics.connected += 1;

        const expected_users = [...authenticated.map(item => item.username), user.username];
        await authenticate(user.client, user.token, expected_users, user.role);
        statistics.authenticated += 1;
        for (const previous of authenticated) {
            await expectOnlineUsers(previous.client, expected_users, `${previous.role}_after_${user.role}`);
        }
        authenticated.push(user);
    }
}

async function relayAndConfirm({sender, recipient, run_id, sequence, statistics}) {
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

    const forwarded = await readJsonFrame(recipient.client, CHAT_FRAME_TYPE, `${sequence}_forwarded`);
    const acknowledgement = await readJsonFrame(sender.client, CHAT_ACK_FRAME_TYPE, `${sequence}_acknowledgement`);

    assert.equal(acknowledgement.local_id, local_id, `${sequence}_ack_local_id`);
    assert.equal(acknowledgement.status, 'accepted', `${sequence}_ack_status`);
    assert.equal(typeof acknowledgement.message_id, 'string', `${sequence}_ack_message_id`);
    assert.ok(acknowledgement.message_id.length > 0, `${sequence}_ack_message_id_empty`);
    assert.ok(Number.isSafeInteger(acknowledgement.server_received_at_ms), `${sequence}_ack_time`);
    statistics.acknowledged += 1;

    assert.equal(forwarded.message_id, acknowledgement.message_id, `${sequence}_forward_message_id`);
    assert.equal(forwarded.local_id, local_id, `${sequence}_forward_local_id`);
    assert.equal(forwarded.from, sender.username, `${sequence}_forward_sender`);
    assert.equal(forwarded.to, recipient.username, `${sequence}_forward_recipient`);
    assert.equal(forwarded.content, content, `${sequence}_forward_content`);
    assert.equal(forwarded.send_at, send_at, `${sequence}_forward_send_at`);
    assert.equal(forwarded.server_received_at_ms, acknowledgement.server_received_at_ms,
        `${sequence}_forward_time`);
    statistics.forwarded += 1;

    recipient.client.writeFrame(DELIVERY_RECEIPT_FRAME_TYPE, JSON.stringify({
        message_id: acknowledgement.message_id
    }));
    const delivered = await readJsonFrame(sender.client, DELIVERY_RECEIPT_FRAME_TYPE, `${sequence}_delivered`);
    assert.equal(delivered.local_id, local_id, `${sequence}_delivered_local_id`);
    assert.equal(delivered.status, 'delivered', `${sequence}_delivered_status`);
    statistics.delivered += 1;
}

async function relayRing(environment, users, statistics) {
    for (let index = 0; index < users.length; index += 1) {
        await relayAndConfirm({
            sender: users[index],
            recipient: users[(index + 1) % users.length],
            run_id: environment.run_id,
            sequence: `${users[index].role}_to_${users[(index + 1) % users.length].role}`,
            statistics
        });
    }
}

// 主动关闭全部已认证 socket 后，服务必须仍存活并保持监听；统计只在 close 事件真正完成后递增。
async function closeUsersAndAssertServerAlive(environment, users, statistics) {
    await Promise.all(users.map(async user => {
        await closeFrameClient(user.client, `${user.role}_normal_close`);
        statistics.closed += 1;
    }));
    await delay(100);
    assert.equal(environment.chat_process.child.exitCode, null, 'chat_server_exited_after_client_close');
    assert.equal(await probeTcpPort(environment.chat_server_port), 'OPEN', 'chat_server_not_listening_after_close');
}

async function readAuthFailure(frame_client, label) {
    const body = await readJsonFrame(frame_client, ERROR_FRAME_TYPE, label);
    assert.equal(body.scope, 'auth', `${label}_scope`);
    return body;
}

module.exports = {
    AUTH_SERVICE_PORT,
    AUTH_FRAME_TYPE,
    CHAT_ACK_FRAME_TYPE,
    CHAT_FRAME_TYPE,
    DELIVERY_RECEIPT_FRAME_TYPE,
    ERROR_FRAME_TYPE,
    OPERATION_TIMEOUT_MS,
    acquireUnusedPort,
    authenticate,
    closeFrameClient,
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    connectFrameClient,
    createChatEnvironment,
    createUsers,
    delay,
    expectOnlineUsers,
    probeTcpPort,
    readAuthFailure,
    readJsonFrame,
    relayAndConfirm,
    relayRing,
    startProcess,
    stopProcess,
    waitForPortClosed,
    waitForProcessOutput,
    withDeadline
};
