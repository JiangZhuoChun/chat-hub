'use strict';

// 为单个异步步骤建立 deadline：真实操作先完成则保留其结果，超时则返回稳定错误码。
async function withDeadline(operation, timeout_ms, label) {
    if (!Number.isInteger(timeout_ms) || timeout_ms <= 0) {
        throw new RangeError('deadline_timeout_invalid');
    }
    if (typeof label !== 'string' || label.trim().length === 0) {
        throw new TypeError('deadline_label_invalid');
    }

    let timeout_id;
    const timeout_promise = new Promise((_, reject) => {
        timeout_id = setTimeout(() => {
            reject(new Error(`${label}_timeout`));
        }, timeout_ms);
    });

    try {
        // Promise.race 只决定等待结果；socket、子进程等真实资源由调用方 finally 清理。
        return await Promise.race([Promise.resolve(operation), timeout_promise]);
    } finally {
        // 操作先完成或先失败时撤销定时器，避免遗留无意义的 timeout 回调。
        clearTimeout(timeout_id);
    }
}

module.exports = {withDeadline};
