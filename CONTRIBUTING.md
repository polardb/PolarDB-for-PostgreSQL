# Contributing

PolarDB for PostgreSQL is an open source project based on PostgreSQL and other open source projects. Our main target is to create a larger community of PostgreSQL. Contributors are welcomed to submit their code and ideas. In a long run, we hope this project can be managed by developers from both inside and outside Alibaba Cloud.

## Before Contributing

- Follow the instructions and sign [CLA](https://gist.github.com/alibaba-oss/151a13b0a72e44ba471119c7eb737d74) of PolarDB for PostgreSQL

## Steps

Here is a checklist to prepare and submit your PR (pull request):

- Create your own Github repository copy by forking `polardb/PolarDB-for-PostgreSQL`.
- Checkout documentations [Advanced Deployment](https://polardb.github.io/polardb-pg-docs/deploying/deploy.html) for how to hack PolarDB-PG.
- Run `make stylecheck` to format your code, and push changes to your personal fork.
- Edit detailed commit message, and create a PR to upstream.
- Wait for all CI checks to pass.
- Wait for review and address all feedbacks.
- Wait for merging.

## Coding Style

### Languages

- PostgreSQL kernel, extension and related tools use C, in order to remain compatibility with community version and to upgrade easily.
- Management related tools can use shell or Perl for efficient development.

### Coding Style

- Coding in C follows PostgreSQL's programing style, such as naming, error message format, control statements, length of lines, comment format, length of functions and global variables. In detail, please refer to [PostgreSQL style](https://www.postgresql.org/docs/current/source.html). Here is some highlines:

  - Code in PostgreSQL should only rely on language features available in the C99 standard
  - Do not use `//` for comments
  - Both, macros with arguments and static inline functions, may be used. The latter is preferred only if the former simplifies coding.
  - Follow BSD C programming conventions

- Programs in shell can follow [Google code conventions](https://google.github.io/styleguide/shellguide.html)
- Program in Perl can follow official [Perl style](https://perldoc.perl.org/perlstyle)

### Code Design and Review

We share the same thought and rules as [Google Open Source Code Review](https://github.com/google/eng-practices/blob/master/review/index.md).

Before submitting code review, please run unit test and pass all tests under `src/test`, such as regress and isolation. Unit tests or function tests should be submitted with code modification.

In addition to code review, this document offers instructions for the whole cycle of high-quality development, from design, implementation, testing, documentation to preparing for code review. Many good questions are asked for critical steps during development, such as about design, function, complexity, testing, naming, documentation, and code review. The documentation summarizes rules for code review as follows. During a code review, you should make sure that:

- The code is well-designed.
- The functionality is good for the users of the code.
- Any UI changes are sensible and look good.
- Any parallel programming is done safely.
- The code isn't more complex than it needs to be.
- The developer isn't implementing things they might need in the future but don't know they need now.
- Code has appropriate unit tests.
- Tests are well-designed.
- The developer used clear names for everything.
- Comments are clear and useful, and mostly explain why instead of what.
- Code is appropriately documented.
- The code conforms to our style guides.
