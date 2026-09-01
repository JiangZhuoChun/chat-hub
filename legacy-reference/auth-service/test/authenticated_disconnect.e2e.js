'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {
    closeFrameClient,
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    createChatEnvironment,
    createUsers,
    delay,
    expectOnlineUsers,
    relayAndConfirm
} = require('./e2e_chat_support');

function countNormalDisconnectLogs(output) {
    return (output.match(/phase=read event=peer_disconnected code=normal_disconnect/g) || []).length;
}

// 用短轮询等到本次断开新增结构化日志；旧启动探针的日志不能当作本次断开的证据。
async function waitForAdditionalDisconnectLog(chat_process, previous_count) {
    const deadline = Date.now() + 5000;
    while (countNormalDisconnectLogs(chat_process.output()) <= previous_count) {
        if (Date.now() >= deadline) {
            throw new Error('authenticated_disconnect_log_timeout');
        }
        await delay(25);
    }
}

test('认证后异常断开只移除该会话，剩余客户端仍可聊天', {timeout: 60000}, async t => {
    const environment = await createChatEnvironment(t);
    const [alice, bob, carol] = await createUsers(environment, 3, 'drop');
    const statistics = {
        connected: 0,
        authenticated: 0,
        sent: 0,
        acknowledged: 0,
        forwarded: 0,
        delivered: 0,
        abruptly_closed: 0,
        normally_closed: 0
    };

    await connectAndAuthenticateUsers(environment, [alice, bob, carol], statistics);
    const previous_disconnect_logs = countNormalDisconnectLogs(environment.chat_process.output());

    // 模拟已认证客户端突然失联；这不是正常业务登出，也不停止 Auth 或 ChatServer 子进程。
    await closeFrameClient(bob.client, 'bob_authenticated_abrupt_close');
    statistics.abruptly_closed += 1;
    await expectOnlineUsers(alice.client, [alice.username, carol.username], 'alice_after_bob_disconnect');
    await expectOnlineUsers(carol.client, [alice.username, carol.username], 'carol_after_bob_disconnect');
    await waitForAdditionalDisconnectLog(environment.chat_process, previous_disconnect_logs);

    // 仍在线的两条会话必须走完整 ACK -> 转发 -> 送达路径，证明断开没有污染路由表。
    await relayAndConfirm({
        sender: alice,
        recipient: carol,
        run_id: environment.run_id,
        sequence: 'alice_to_carol_after_bob_disconnect',
        statistics
    });

    const close_statistics = {closed: 0};
    await closeUsersAndAssertServerAlive(environment, [alice, carol], close_statistics);
    statistics.normally_closed = close_statistics.closed;

    assert.deepEqual(statistics, {
        connected: 3,
        authenticated: 3,
        sent: 1,
        acknowledged: 1,
        forwarded: 1,
        delivered: 1,
        abruptly_closed: 1,
        normally_closed: 2
    });
    t.diagnostic(
        'scenario=authenticated_disconnect connected=3 authenticated=3 abruptly_closed=1 '
        + 'sent=1 acknowledged=1 forwarded=1 delivered=1 normally_closed=2'
    );
});
