CREATE TABLE configurations (
       id serial PRIMARY KEY,
       service_name varchar NOT NULL,
       client_id uuid NOT NULL,
       rate_limit int NOT NULL,
       window_size int NOT NULL
)
