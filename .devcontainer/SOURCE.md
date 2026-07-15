# Источник этого .devcontainer

`Dockerfile` и `devcontainer.json` в этой папке — точная копия
`coder-deploy/workspace-image/{Dockerfile,devcontainer.json}` из
инфраструктурного промта (тот же репозиторий верхнего уровня, вне
`bmm-translib/`). Файлы скопированы, а не написаны заново, по прямому
указанию задания ("переиспользовать из инфраструктурного промта, не
создавать заново").

Если нужно поменять образ (добавить TBB, заголовки libbrial и т.п. — см.
известные пробелы в `core/CONVENTIONS.md`, пп. 5–6) — правьте
`coder-deploy/workspace-image/Dockerfile` в исходном месте и копируйте
заново сюда, а не расходитесь в две независимые версии.

Реальный build-контекст, который использует Coder (см.
`coder-deploy/template/main.tf`), — это `coder-deploy/workspace-image/`
напрямую (образ `genetica-boolean-lib:latest` собирается оттуда через
Terraform/Docker, не через discovery `.devcontainer/` от VS Code). Копия в
этой папке — для (а) IDE-интеграции при открытии `bmm-translib/` отдельно
от остального монорепозитория, и (б) чтобы структура `bmm-translib/`
соответствовала зафиксированному заданию.
