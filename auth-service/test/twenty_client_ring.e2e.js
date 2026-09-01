'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    createChatEnvironment,
    createUsers,
    relayRing
} = require('./e2e_chat_support');

// W13 的 20 客户端门槛：恰好 20 次真实 /login，与测试 Auth 的 IP limit 对齐。
// 这验证的是受控连接和消息计数守恒，不把一次成功误写成吞吐或容量上限结论。
test('二十个真实客户端认证、环形聊天并关闭后 ChatServer 仍保持可用', {timeout: 180000}, async t => {
    const environment = await createChatEnvironment(t);
    // username 合同上限为 20 字节；与 run_id 组合后用 t01…t20 仍保持唯一且不越界。
    const users = await createUsers(environment, 20, 't');
    const statistics = {
        connected: 0,
        authenticated: 0,
        sent: 0,
        acknowledged: 0,
        forwarded: 0,
        delivered: 0,
        closed: 0
    };

    await connectAndAuthenticateUsers(environment, users, statistics);
    await relayRing(environment, users, statistics);
    await closeUsersAndAssertServerAlive(environment, users, statistics);

    // 每个客户端在本场景中恰好贡献一次连接、认证、发送、接收、回执和关闭。
    assert.deepEqual(statistics, {
        connected: 20,
        authenticated: 20,
        sent: 20,
        acknowledged: 20,
        forwarded: 20,
        delivered: 20,
        closed: 20
    });
    t.diagnostic(
        'scenario=twenty_client_ring connected=20 authenticated=20 sent=20 '
        + 'acknowledged=20 forwarded=20 delivered=20 closed=20'
    );
});
