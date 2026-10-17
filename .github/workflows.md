# ShannonBase Workflow

## AI pull request review

Every pull request gets one AI review (a summary plus inline comments) from the
[AI PR Review](./workflows/pr-review.yml) workflow. It runs on `opened`,
`synchronize`, `reopened`, and `ready_for_review`, skips drafts and bots, and
never checks out or executes PR code: it reads the diff through the GitHub API.

- Comment `/review` on a pull request to request another review. Only
  `OWNER`, `MEMBER`, and `COLLABORATOR` comments trigger a run.
- Add the `skip-ai-review` label to opt a pull request out.
- Review rules live in [pr-review-prompt.md](./pr-review-prompt.md).

Setup: add the repository secret `PR_REVIEW_API_KEY` with a key for any
OpenAI-compatible provider. The reviewer defaults to OpenAI
(`https://api.openai.com/v1`, `gpt-4o-mini`); set the repository variables
`PR_REVIEW_BASE_URL` and `PR_REVIEW_MODEL` to use another provider, for example:

| Provider | `PR_REVIEW_BASE_URL` | `PR_REVIEW_MODEL` |
| --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-4o-mini` |
| DeepSeek | `https://api.deepseek.com/v1` | `deepseek-chat` |
| OpenRouter | `https://openrouter.ai/api/v1` | `openrouter/auto` |

The job fails with an explicit error until `PR_REVIEW_API_KEY` is set.

## Release

ShannonBase make new releases via workflow [Release](./workflows/release.yml).

There are three ways to trigger a new release:

- Push a new tag
- On Schedule (every day at 00:00 UTC+8)
- Manually triggered

If new tags pushed, we use this the new tag name as release name. Otherwise, we will generate a new nightly tag.

For example:

- If current latest release is `v0.2.0`, we will generate a nightly tag `v0.2.0-nightly`
- If current latest release is `v0.2.0-nightly`, we will generate a new tag `v0.2.0-nightly`

For every release, we will:

- Create a new release on GitHub.
- Build binaries and pack them on Linux and MacOS.
- Upload built packages to GitHub Releases, AWS S3 bucket and Docker Registry.

We are adopting github native release notes generation which controlled by [release.yml](./release.yml).