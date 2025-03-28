import time

import jwt
import requests

from .utils import Shell


class GHAuth:

    @classmethod
    def _get_installation_id(cls, jwt_token: str) -> int:
        headers = {
            "Authorization": f"Bearer {jwt_token}",
            "Accept": "application/vnd.github.v3+json",
        }
        response = requests.get(
            "https://api.github.com/app/installations", headers=headers, timeout=10
        )
        response.raise_for_status()
        data = response.json()
        for installation in data:
            # TODO: make account login configurable
            if installation["account"]["login"] == "ClickHouse":
                installation_id = installation["id"]  # type: int
                break
        else:
            raise KeyError("No installations found")

        return installation_id

    @classmethod
    def _get_access_token_by_jwt(cls, jwt_token: str, installation_id: int) -> str:
        headers = {
            "Authorization": f"Bearer {jwt_token}",
            "Accept": "application/vnd.github.v3+json",
        }
        response = requests.post(
            f"https://api.github.com/app/installations/{installation_id}/access_tokens",
            headers=headers,
            timeout=10,
        )
        response.raise_for_status()
        token = response.json()["token"]  # type: str
        return token

    @classmethod
    def _get_access_token(cls, private_key: str, app_id: str) -> str:
        payload = {
            "iat": int(time.time()) - 60,
            "exp": int(time.time()) + (10 * 60),
            "iss": app_id,
        }

        jwt_instance = jwt.PyJWT()
        encoded_jwt = jwt_instance.encode(payload, private_key, algorithm="RS256")
        installation_id = cls._get_installation_id(encoded_jwt)
        return cls._get_access_token_by_jwt(encoded_jwt, installation_id)

    @classmethod
    def auth(cls, app_key, app_id) -> None:
        access_token = cls._get_access_token(app_key, app_id)
        Shell.check(f"echo {access_token} | gh auth login --with-token", strict=True)


# if __name__ == "__main__":
#     pem = Secret.Config(
#         name="woolenwolf_gh_app.clickhouse-app-key",
#         type=Secret.Type.AWS_SSM_SECRET,
#     ).get_value()
#     app_id = Secret.Config(
#         name="woolenwolf_gh_app.clickhouse-app-id",
#         type=Secret.Type.AWS_SSM_SECRET,
#     ).get_value()
#     GHAuth.auth(app_id, pem)
