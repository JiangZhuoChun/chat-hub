'use strict';

const REQUEST_TIMEOUT_MS = 5000;

function withRequestTimeout(options = {}) {
    const request_options = {...options};
    if (request_options.signal === undefined) {
        request_options.signal = AbortSignal.timeout(REQUEST_TIMEOUT_MS);
    }
    return request_options;
}

async function requestJson(base_url, route, options = {}) {
    const response = await fetch(
        `${base_url}${route}`,
        withRequestTimeout(options)
    );
    return {
        status: response.status,
        body: await response.json()
    };
}

async function requestJsonWithRetryAfter(base_url, route, options = {}) {
    const response = await fetch(
        `${base_url}${route}`,
        withRequestTimeout(options)
    );
    return {
        status: response.status,
        body: await response.json(),
        retryAfter: response.headers.get('retry-after')
    };
}

module.exports = {
    requestJson,
    requestJsonWithRetryAfter
};
